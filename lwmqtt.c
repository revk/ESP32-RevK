// Light weight MQTT client
// QoS 0 only, no queuing or resending (using TCP to do that for us)
// Live sending to TCP for outgoing messages
// Simple callback for incoming messages
// Automatic reconnect
static const char __attribute__((unused)) * TAG = "LWMQTT";

#include "revk.h"
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_tls.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_tls.h"
#ifdef  CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "esp8266_tls_compat.h"

#ifdef	CONFIG_REVK_MQTT_SERVER
#warning MQTT server code is not complete
#endif

struct lwmqtt_s
{                               // mallocd copies
   lwmqtt_callback_t *callback;
   void *arg;
   char *hostname;
   char *tlsname;
   unsigned short port;
   unsigned short connectlen;
   unsigned char *connect;
   SemaphoreHandle_t mutex;     // atomic send mutex
   esp_tls_t *tls;              // Connection handle
   int sock;                    // Connection socket
   unsigned short keepalive;
   unsigned short seq;
   uint32_t connecttime;        // Time of connect
   uint8_t backoff:4;           // Reconnect backoff
   uint8_t failed:3;            // Login received error
   uint8_t running:1;           // Should still run
   uint8_t close:1;             // Close this handle cleanly and reconnect
   uint8_t server:1;            // This is a server
   uint8_t connected:1;         // Login sent/received
   uint8_t ca_cert_ref:1;       // The _buf below is not malloc'd
   uint8_t our_cert_ref:1;      // The _buf below is not malloc'd
   uint8_t our_key_ref:1;       // The _buf below is not malloc'd
   uint8_t hostname_ref:1;      // The buf below is not malloc'd
   uint8_t tlsname_ref:1;       // The buf below is not malloc'd
   uint8_t dnsipv6:1;           // DNS has IPv6
   uint8_t ipv6:1;              // Connection is IPv6
   void *ca_cert_buf;           // For checking server
   int ca_cert_bytes;
   void *our_cert_buf;          // For auth
   int our_cert_bytes;
   void *our_key_buf;           // For auth
   int our_key_bytes;
     esp_err_t (*crt_bundle_attach) (void *conf);
};

#define	hread(handle,buf,len)	(handle->tls?esp_tls_conn_read(handle->tls,buf,len):read(handle->sock,buf,len))

static int
hwrite (lwmqtt_t handle, uint8_t * buf, int len)
{                               // Send (all of) a block
   int pos = 0;
   while (pos < len)
   {
      int sent =
         (handle->tls ? esp_tls_conn_write (handle->tls, buf + pos, len - pos) : write (handle->sock, buf + pos, len - pos));
      if (sent <= 0)
         return sent;
      pos += sent;
   }
   return pos;
}

static void *
handle_free (lwmqtt_t handle)
{
   if (handle)
   {
      freez (handle->connect);
      if (!handle->hostname_ref)
         freez (handle->hostname);
      if (!handle->tlsname_ref)
         freez (handle->tlsname);
      if (!handle->ca_cert_ref)
         freez (handle->ca_cert_buf);
      if (!handle->our_cert_ref)
         freez (handle->our_cert_buf);
      if (!handle->our_key_ref)
         freez (handle->our_key_buf);
      if (handle->mutex)
         vSemaphoreDelete (handle->mutex);
      freez (handle);
   }
   return NULL;
}

void
handle_close (lwmqtt_t handle)
{
   esp_tls_t *tls = handle->tls;
   int sock = handle->sock;
   handle->tls = NULL;
   handle->sock = -1;
   if (tls)
   {                            // TLS
      if (handle->server)
      {
#ifdef CONFIG_ESP_TLS_SERVER
         esp_tls_server_session_delete (tls);
#endif
         close (sock);
      } else
         esp_tls_conn_destroy (tls);
   } else if (sock >= 0)
      close (sock);
   usleep (100000);
}

static int
handle_certs (lwmqtt_t h, uint8_t ca_cert_ref, int ca_cert_bytes, void *ca_cert_buf, uint8_t our_cert_ref, int our_cert_bytes,
              void *our_cert_buf, uint8_t our_key_ref, int our_key_bytes, void *our_key_buf)
{
   int fail = 0;
   if (ca_cert_bytes && ca_cert_buf)
   {
      h->ca_cert_bytes = ca_cert_bytes;
      if ((h->ca_cert_ref = ca_cert_ref))
         h->ca_cert_buf = ca_cert_buf;  // No malloc as reference will stay valid
      else if (!(h->ca_cert_buf = mallocspi (ca_cert_bytes)))
         fail++;                // malloc failed
      else
         memcpy (h->ca_cert_buf, ca_cert_buf, ca_cert_bytes);
   }
   if (our_cert_bytes && our_cert_buf)
   {
      h->our_cert_bytes = our_cert_bytes;
      if ((h->our_cert_ref = our_cert_ref))
         h->our_cert_buf = our_cert_buf;        // No malloc as reference will stay valid
      else if (!(h->our_cert_buf = mallocspi (our_cert_bytes)))
         fail++;                // malloc failed
      else
         memcpy (h->our_cert_buf, our_cert_buf, our_cert_bytes);
   }
   if (our_key_bytes && our_key_buf)
   {
      h->our_key_bytes = our_key_bytes;
      if ((h->our_key_ref = our_cert_ref))
         h->our_key_buf = our_key_buf;  // No malloc as reference will stay valid
      else if (!(h->our_key_buf = mallocspi (our_key_bytes)))
         fail++;                // malloc failed
      else
         memcpy (h->our_key_buf, our_key_buf, our_key_bytes);
   }
   return fail;
}

static void client_task (void *pvParameters);
#ifdef  CONFIG_REVK_MQTT_SERVER
static void listen_task (void *pvParameters);
#endif

// Create a connection
lwmqtt_t
lwmqtt_client (lwmqtt_client_config_t * config)
{
   if (!config || !config->hostname)
      return NULL;
   lwmqtt_t handle = mallocspi (sizeof (*handle));
   if (!handle)
      return handle_free (handle);
   memset (handle, 0, sizeof (*handle));
   handle->sock = -1;
   handle->callback = config->callback;
   handle->arg = config->arg;
   handle->keepalive = config->keepalive ? : 60;
   if ((handle->hostname_ref = config->hostname_ref))
      handle->hostname = (void *) config->hostname;
   else if (!(handle->hostname = strdup (config->hostname)))
      return handle_free (handle);
   handle->port = (config->port ? : (config->ca_cert_bytes || config->crt_bundle_attach) ? 8883 : 1883);
   if ((handle->tlsname_ref = config->tlsname_ref))
      handle->tlsname = (void *) config->tlsname;
   else if (config->tlsname && *config->tlsname && !(handle->tlsname = strdup (config->tlsname)))
      return handle_free (handle);
   // Make connection message
   int mlen = 6 + 1 + 1 + 2 + strlen (config->client ? : "");
   if (config->plen < 0)
      config->plen = strlen ((char *) config->payload ? : "");
   if (config->topic)
      mlen += 2 + strlen (config->topic) + 2 + config->plen;
   if (config->username)
   {
      mlen += 2 + strlen (config->username);
      if (config->password)
         mlen += 2 + strlen (config->password);
   }
   if (handle_certs
       (handle, config->ca_cert_ref, config->ca_cert_bytes, config->ca_cert_buf, config->client_cert_ref, config->client_cert_bytes,
        config->client_cert_buf, config->client_key_ref, config->client_key_bytes, config->client_key_buf))
      return handle_free (handle);      // Nope
   handle->crt_bundle_attach = config->crt_bundle_attach;
   if (mlen >= 128 * 128)
      return handle_free (handle);      // Nope
   mlen += 2;                   // keepalive
   if (mlen >= 128)
      mlen++;                   // two byte len
   mlen += 2;                   // header and one byte len
   if (!(handle->connect = mallocspi (mlen)))
      return handle_free (handle);
   unsigned char *p = handle->connect;
   void str (int l, const char *s)
   {
      if (l < 0)
         l = strlen (s ? : "");
      *p++ = l >> 8;
      *p++ = l;
      if (l && s)
         memcpy (p, s, l);
      p += l;
   }
   *p++ = 0x10;                 // connect
   if (mlen > 129)
   {                            // Two byte len
      *p++ = (((mlen - 3) & 0x7F) | 0x80);
      *p++ = ((mlen - 3) >> 7);
   } else
      *p++ = mlen - 2;          // 1 byte len
   str (4, "MQTT");
   *p++ = 4;                    // protocol level
   *p = 0x02;                   // connect flags (clean)
   if (config->username)
   {
      *p |= 0x80;               // Username
      if (config->password)
         *p |= 0x40;            // Password
   }
   if (config->topic)
   {
      *p |= 0x04;               // Will
      if (config->retain)
         *p |= 0x20;            // Will retain
   }
   p++;
   *p++ = handle->keepalive >> 8;       // keep alive
   *p++ = handle->keepalive;
   str (-1, config->client);    // Client ID
   if (config->topic)
   {                            // Will
      str (-1, config->topic);  // Topic
      str (config->plen, (void *) config->payload);     // Payload
   }
   if (config->username)
   {
      str (-1, config->username);
      if (config->password)
         str (-1, config->password);
   }
   assert ((p - handle->connect) == mlen);
   handle->connectlen = mlen;
   handle->mutex = xSemaphoreCreateBinary ();
   xSemaphoreGive (handle->mutex);
   handle->running = 1;
   TaskHandle_t task_id = NULL;
   xTaskCreate (client_task, "mqtt-client", 4 * 1024, (void *) handle, 2, &task_id);
   return handle;
}

#ifdef	CONFIG_REVK_MQTT_SERVER
// Start a server
lwmqtt_t
lwmqtt_server (lwmqtt_server_config_t * config)
{
   if (!config)
      return NULL;
   lwmqtt_t handle = mallocspi (sizeof (*handle));
   if (!handle)
      return handle_free (handle);
   memset (handle, 0, sizeof (*handle));
   handle->callback = config->callback;
   handle->port = (config->port ? : config->ca_cert_bytes ? 8883 : 1883);
   if (handle_certs
       (handle, config->ca_cert_ref, config->ca_cert_bytes, config->ca_cert_buf, config->server_cert_ref, config->server_cert_bytes,
        config->server_cert_buf, config->server_key_ref, config->server_key_bytes, config->server_key_buf))
      return handle_free (handle);
   handle->running = 1;
   TaskHandle_t task_id = NULL;
   xTaskCreate (listen_task, "mqtt-listen", 3 * 1024, (void *) handle, 2, &task_id);
   return handle;
}
#endif

// End connection - actually freed later as part of task. Will do a callback when closed if was connected
// NULLs the passed handle - do not use handle after this call
void
lwmqtt_end (lwmqtt_t * handle)
{
   if (!handle || !*handle)
      return;
   if ((*handle)->running)
   {
      ESP_LOGD (TAG, "Ending");
      (*handle)->running = 0;
   }
   *handle = NULL;
}

void
lwmqtt_reconnect (lwmqtt_t handle)
{
   if (!handle)
      return;
   if (handle->running)
   {
      ESP_LOGD (TAG, "Closing to reconnect");
      handle->close = 1;
   }
}

void
lwmqtt_reconnect6 (lwmqtt_t handle)
{
   if (!handle)
      return;
   if (handle->running && (handle->dnsipv6 || handle->tls) && !handle->ipv6)
      handle->close = 1;        // Reconnect as IPv6 (we don't know for TLS so reconnect anyway)
}

// Subscribe (return is non null error message if failed)
const char *
lwmqtt_subscribeub (lwmqtt_t handle, const char *topic, char unsubscribe)
{
   const char *ret = NULL;
   if (!handle)
      ret = "No handle";
   else if (handle->server)
      ret = "We are server";
   else
   {
      int tlen = strlen (topic ? : "");
      int mlen = 2 + 2 + tlen;
      if (!unsubscribe)
         mlen++;                // QoS requested
      if (mlen >= 128 * 128)
         ret = "Too big";
      else
      {
         if (mlen >= 128)
            mlen++;             // two byte len
         mlen += 2;             // header and one byte len
         unsigned char *buf = mallocspi (mlen);
         if (!buf)
            ret = "Malloc";
         else
         {
            if (!xSemaphoreTake (handle->mutex, portMAX_DELAY))
               ret = "Failed to get lock";
            else
            {
               if (handle->sock < 0)
                  ret = "Not connected";
               else
               {
                  unsigned char *p = buf;
                  *p++ = (unsubscribe ? 0xA2 : 0x82);   // subscribe/unsubscribe
                  if (mlen > 129)
                  {             // Two byte len
                     *p++ = (((mlen - 3) & 0x7F) | 0x80);
                     *p++ = ((mlen - 3) >> 7);
                  } else
                     *p++ = mlen - 2;   // 1 byte len
                  if (!++(handle->seq))
                     handle->seq++;     // Non zero
                  *p++ = handle->seq >> 8;
                  *p++ = handle->seq;
                  *p++ = tlen >> 8;
                  *p++ = tlen;
                  if (tlen)
                     memcpy (p, topic, tlen);
                  p += tlen;
                  if (!unsubscribe)
                     *p++ = 0x00;       // QoS requested
                  assert ((p - buf) == mlen);
                  if (hwrite (handle, buf, mlen) < mlen)
                     ret = "Failed to send";
               }
               xSemaphoreGive (handle->mutex);
            }
            freez (buf);
         }
      }
   }
   if (ret)
      ESP_LOGD (TAG, "Sub/unsub: %s", ret);
   return ret;
}

// Send (return is non null error message if failed)
const char *
lwmqtt_send_full (lwmqtt_t handle, int tlen, const char *topic, int plen, const unsigned char *payload, char retain)
{
   const char *ret = NULL;
   if (!handle)
      ret = "No handle";
   else
   {
      if (tlen < 0)
         tlen = strlen (topic ? : "");
      if (plen < 0)
         plen = strlen ((char *) payload ? : "");
      int mlen = 2 + tlen + plen;
      if (mlen >= 128 * 128)
         ret = "Too big";
      else
      {
         if (mlen >= 128)
            mlen++;             // two byte len
         mlen += 2;             // header and one byte len
         unsigned char *buf = mallocspi (mlen);
         if (!buf)
            ret = "Malloc";
         else
         {
            if (!xSemaphoreTake (handle->mutex, portMAX_DELAY))
               ret = "Failed to get lock";
            else
            {
               if (handle->sock < 0)
                  ret = "Not connected";
               else
               {
                  unsigned char *p = buf;
                  *p++ = 0x30 + (retain ? 1 : 0);       // message
                  if (mlen > 129)
                  {             // Two byte len
                     *p++ = (((mlen - 3) & 0x7F) | 0x80);
                     *p++ = ((mlen - 3) >> 7);
                  } else
                     *p++ = mlen - 2;   // 1 byte len
                  *p++ = tlen >> 8;
                  *p++ = tlen;
                  if (tlen)
                     memcpy (p, topic, tlen);
                  p += tlen;
                  if (plen && payload)
                     memcpy (p, payload, plen);
                  p += plen;
                  assert ((p - buf) == mlen);
                  if (hwrite (handle, buf, mlen) < mlen)
                     ret = "Failed to send";
               }
               xSemaphoreGive (handle->mutex);
            }
            freez (buf);
         }
      }
   }
   if (ret)
      ESP_LOGD (TAG, "Send: %s", ret);
   return ret;
}

static void
lwmqtt_loop (lwmqtt_t handle)
{
   // Handle rx messages
   unsigned char *buf = 0;
   int buflen = 0;
   int pos = 0;
   uint32_t kacheck = uptime () + 60;   // Response time check
   uint32_t ka = uptime () + (handle->server ? 5 : handle->keepalive);  // Server does not know KA initially
   while (handle->running && !handle->close)
   {                            // Loop handling messages received, and timeouts
      int need = 0;
      if (pos < 2)
         need = 2;
      else if (!(buf[1] & 0x80))
         need = 2 + buf[1];     // One byte len
      else if (pos < 3)
         need = 3;
      else if (!(buf[2] & 0x80))
         need = 3 + (buf[2] << 7) + (buf[1] & 0x7F);    // Two byte len
      else
      {
         ESP_LOGE (TAG, "Silly len %02X %02X %02X", buf[0], buf[1], buf[2]);
         break;
      }
      if (pos < need)
      {
         uint32_t now = uptime ();
         if (now >= ka)
         {
            if (handle->server)
               break;           // timeout
            // client, so send ping - do so regularly regardless as we want pingresp regularly to detect down as a client.
            uint8_t b[] = { 0xC0, 0x00 };       // Ping
            xSemaphoreTake (handle->mutex, portMAX_DELAY);
            hwrite (handle, b, sizeof (b));
            xSemaphoreGive (handle->mutex);
            ka = uptime () + handle->keepalive; // Client KA next
            kacheck = uptime () + 10;   // Expect KA resp
         } else if (kacheck && kacheck < uptime ())
         {                      // only set for client anyway
            ESP_LOGE (TAG, "KA fail");
            break;
         }
         if (!handle->tls || esp_tls_get_bytes_avail (handle->tls) <= 0)
         {                      // Wait for data to arrive
            fd_set r,
              e;
            FD_ZERO (&r);
            FD_SET (handle->sock, &r);
            FD_ZERO (&e);
            FD_SET (handle->sock, &e);
            struct timeval to = { 1, 0 };       // Keeps us checking running but is light load at once a second
            int sel = select (handle->sock + 1, &r, NULL, &e, &to);
            if (sel < 0)
            {
               ESP_LOGE (TAG, "Select failed");
               break;
            }
            if (FD_ISSET (handle->sock, &e))
            {
               ESP_LOGE (TAG, "Closed");
               break;
            }
            if (!FD_ISSET (handle->sock, &r))
               continue;        // Nothing waiting
         }
         if (need > buflen)
         {                      // Make sure we have enough space
            buf = realloc (buf, (buflen = need) + 1);   // One more to allow extra null on end in all cases
            if (!buf)
            {
               ESP_LOGE (TAG, "realloc fail %d", need);
               break;
            }
         }
         int got = hread (handle, buf + pos, need - pos);
         if (got <= 0)
         {
            ESP_LOGI (TAG, "Connection closed");
            break;              // Error or close
         }
         pos += got;
         continue;
      }
      kacheck = 0;              // We got something (does not have to be pingresp)
      if (handle->server)
         ka = uptime () + handle->keepalive * 3 / 2;    // timeout for client resent on message received
      unsigned char *p = buf + 1,
         *e = buf + pos;
      while (p < e && (*p & 0x80))
         p++;
      p++;
#ifdef CONFIG_REVK_MQTT_SERVER
      if (handle->server && !handle->connected && (*buf >> 4) != 1)
         break;                 // Expect login as first message
#endif
      switch (*buf >> 4)
      {
      case 1:
#ifdef CONFIG_REVK_MQTT_SERVER
         if (!handle->server)
            break;
         handle->connected = 1;
         ESP_LOGI (TAG, "Connected incoming %d", handle->port);
         // TODO incoming connect
         handle->keepalive = 10;        // TODO get from message
         uint8_t b[4] = { 0x20 };       // conn ack
         xSemaphoreTake (handle->mutex, portMAX_DELAY);
         hwrite (handle, b, sizeof (b));
         xSemaphoreGive (handle->mutex);
#endif
         break;
      case 2:                  // conack
         if (handle->server)
            break;
         if (p[1])
         {                      // Failed
            ESP_LOGI (TAG, "Connect failed %s:%d code %d", handle->hostname, handle->port, p[1]);
            handle->failed = (p[1] > 7 ? 7 : p[1]);
         } else
         {
            ESP_LOGI (TAG, "Connect ack  %s:%d", handle->hostname, handle->port);
            handle->failed = 0;
            handle->backoff = 0;
            handle->connected = 1;
            handle->connecttime = uptime ();
            if (handle->callback)
               handle->callback (handle->arg, NULL, strlen (handle->hostname), (void *) handle->hostname);
         }
         break;
      case 3:                  // pub
         {                      // Topic
            int tlen = (p[0] << 8) + p[1];
            p += 2;
            char *topic = (char *) p;
            p += tlen;
            unsigned short id = 0;
            if (*buf & 0x06)
            {
               id = (p[0] << 8) + p[1];
               p += 2;
            }
            if (p > e)
            {
               ESP_LOGE (TAG, "Bad msg");
               break;
            }
            if (*buf & 0x06)
            {                   // reply
               uint8_t b[4] = { (*buf & 0x4) ? 0x50 : 0x40, 2, id >> 8, id };
               xSemaphoreTake (handle->mutex, portMAX_DELAY);
               hwrite (handle, b, sizeof (b));
               xSemaphoreGive (handle->mutex);
            }
            int plen = e - p;
            if (handle->callback)
            {
               if (plen && !(*buf & 0x06))
               {                // Move back a byte for null termination to be added without hitting payload
                  memmove (topic - 1, topic, tlen);
                  topic--;
               }
               topic[tlen] = 0;
               p[plen] = 0;
               handle->callback (handle->arg, topic, plen, p);
            }
         }
         break;
      case 4:                  // puback - no action as we don't use non QoS 0
         break;
      case 5:                  // pubrec - not expected as we don't use non QoS 0
         if (handle->server)
            break;
         {
            uint8_t b[4] = { 0x60, p[0], p[1] };
            xSemaphoreTake (handle->mutex, portMAX_DELAY);
            hwrite (handle, b, sizeof (b));
            xSemaphoreGive (handle->mutex);
         }
         break;
      case 6:                  // pubcomp - no action as we don't use non QoS 0
         break;
      case 8:                  // sub
#ifdef CONFIG_REVK_MQTT_SERVER
         if (!handle->server)
            break;
         // TODO
#endif
         break;
      case 9:                  // suback - no action
         break;
      case 10:                 // unsub - no action
         break;
      case 11:                 // unsuback - ok
         if (handle->server)
            break;
         break;
      case 12:                 // ping (no action as resets ka anyway)
         break;
      case 13:                 // pingresp
         break;
#ifdef CONFIG_REVK_MQTT_SERVER
      case 14:                 // disconnect
         ESP_LOGE (TAG, "Client disconnected");
         break;
#endif
      default:
         ESP_LOGE (TAG, "Unknown MQTT %02X (%d)", *buf, pos);
      }
      pos = 0;
   }
   handle->connected = 0;
   freez (buf);
   if (!handle->server && (handle->close || !handle->running))
   {                            // Close connection - as was clean
      ESP_LOGE (TAG, "Closed cleanly%s", handle->close ? " to reconnect" : "");
      uint8_t b[] = { 0xE0, 0x00 };     // Disconnect cleanly
      xSemaphoreTake (handle->mutex, portMAX_DELAY);
      hwrite (handle, b, sizeof (b));
      xSemaphoreGive (handle->mutex);
   }
   handle_close (handle);
   handle->close = 0;
   if (handle->callback)
      handle->callback (handle->arg, NULL, 0, NULL);
}

static void
client_task (void *pvParameters)
{
   lwmqtt_t handle = pvParameters;
   if (!handle)
   {
      vTaskDelete (NULL);
      return;
   }
   handle->backoff = 0;
   while (handle->running)
   {                            // Loop connecting and trying repeatedly
      handle->sock = -1;
      char *hostname = strdup (handle->hostname);
      uint16_t port = handle->port;
      {                         // Port suffix
         char *p = hostname + strlen (hostname);
         while (p > hostname && p[-1] >= '0' && p[-1] <= '9')
            p--;
         if (p > hostname && *p > '0' && *p <= '9' && p[-1] == ':')
         {
            port = atoi (p);
            *--p = 0;
         }
      }
      // Connect
      ESP_LOGI (TAG, "Connecting %s:%d", hostname, port);
      // Can connect using TLS or non TLS with just sock set instead
      if (revk_has_ip ())
      {
         if (handle->ca_cert_bytes || handle->crt_bundle_attach)
         {
            int tryconnect (uint8_t ip6)
            {
               if (handle->sock >= 0)
                  return 1;     // connected already
               esp_tls_t *tls = NULL;
               esp_tls_cfg_t cfg = {
                  .cacert_buf = handle->ca_cert_buf,
                  .cacert_bytes = handle->ca_cert_bytes,
                  .common_name = handle->tlsname,
                  .clientcert_buf = handle->our_cert_buf,
                  .clientcert_bytes = handle->our_cert_bytes,
                  .clientkey_buf = handle->our_key_buf,
                  .clientkey_bytes = handle->our_key_bytes,
                  .crt_bundle_attach = handle->crt_bundle_attach,
                  .addr_family = (ip6 ? ESP_TLS_AF_INET6 : ESP_TLS_AF_INET),
               };
               tls = esp_tls_init ();
               if (esp_tls_conn_new_sync (hostname, strlen (hostname), port, &cfg, tls) != 1)
               {
                  free (tls);
                  return 0;
               }
               handle->tls = tls;
               esp_tls_get_conn_sockfd (handle->tls, &handle->sock);
               if (ip6)
               {
                  handle->ipv6 = 1;
                  handle->close = 0;
               }
               return 1;
            }
            tryconnect (1);     // Explicit try IPv6 first
            tryconnect (0);
         } else
         {                      // Non TLS
            char sport[6];
            snprintf (sport, sizeof (sport), "%d", port);
            int tryconnect (uint8_t ip6)
            {
               if (handle->sock >= 0)
                  return 1;     // connected already
             struct addrinfo base = { ai_family: ip6 ? AF_INET6 : AF_INET, ai_socktype:SOCK_STREAM };
               struct addrinfo *a = 0,
                  *p = NULL;
               if (!getaddrinfo (hostname, sport, &base, &a) && a)
               {
#if 0                           // Debug log the getaddrinfo result - it seems UNSPEC after say IP6 give only IP6,so we need to check 6 and 4 separately
                  ESP_LOGE (TAG, "getaddrinfo %s %s", ip6 ? "IPv6" : "IPv4", hostname);
                  for (p = a; p; p = p->ai_next)
                  {
                     char from[INET6_ADDRSTRLEN + 1] = "";
                     if (p->ai_family == AF_INET)
                        inet_ntop (p->ai_family, &((struct sockaddr_in *) (p->ai_addr))->sin_addr, from, sizeof (from));
                     else
                        inet_ntop (p->ai_family, &((struct sockaddr_in6 *) (p->ai_addr))->sin6_addr, from, sizeof (from));
                     ESP_LOGE (TAG, "%s", from);
                  }
#endif
                  for (p = a; p && !handle->dnsipv6; p = p->ai_next)
                     if (p->ai_family == AF_INET6)
                        handle->dnsipv6 = 1;
                  for (p = a; p; p = p->ai_next)
                  {
                     if (p->ai_family == AF_INET && (ip6 || !revk_has_ipv4 ()))
                        continue;
                     if (p->ai_family == AF_INET6 && (!ip6 || !revk_has_ipv6 ()))
                        continue;
                     handle->sock = socket (p->ai_family, p->ai_socktype, p->ai_protocol);
                     if (handle->sock < 0)
                        continue;
#if 0
                     {          // Debug that we are trying
                        char from[INET6_ADDRSTRLEN + 1] = "";
                        if (p->ai_family == AF_INET)
                           inet_ntop (p->ai_family, &((struct sockaddr_in *) (p->ai_addr))->sin_addr, from, sizeof (from));
                        else
                           inet_ntop (p->ai_family, &((struct sockaddr_in6 *) (p->ai_addr))->sin6_addr, from, sizeof (from));
                        ESP_LOGE (TAG, "Try connect %s (backoff %d)", from, handle->backoff);
                     }
#endif
                     if (connect (handle->sock, p->ai_addr, p->ai_addrlen))
                     {
                        close (handle->sock);
                        handle->sock = -1;
                        continue;
                     }
                     // Connected
                     if (ip6)
                     {          // IPv6 connected
                        handle->ipv6 = 1;       // Is IPv6
                        handle->close = 0;      // We only close to force IPv6, so cancel closing
                     }
                     break;
                  }
               }
               if (a)
                  freeaddrinfo (a);
               if (handle->sock < 0)
                  return 0;     // Not  connected
               return 1;        // Worked
            }
            tryconnect (1);     // Explicit try IPv6 first
            tryconnect (0);
         }
      }
      if (handle->backoff < 10)
         handle->backoff++;     // 100 seconds max
      if (!revk_has_ip ())
         handle->backoff = 0;   // We did not try even
      else if (handle->sock < 0)
      {                         // Failed before we even start
         ESP_LOGI (TAG, "Could not connect to %s:%d", hostname, port);
         if (handle->callback)
            handle->callback (handle->arg, NULL, 0, NULL);
      } else
      {
         ESP_LOGE (TAG, "Connected %s:%d%s", hostname, port, handle->ipv6 ? " (IPv6)" : handle->dnsipv6 ? " (Not IPv6)" : "");
         hwrite (handle, handle->connect, handle->connectlen);
         lwmqtt_loop (handle);
         handle->backoff = 0;
         handle->dnsipv6 = 0;
         handle->ipv6 = 0;
      }
      free (hostname);
      // On ESP32 uint32_t, returned by this func, appears to be long, while on ESP8266 it's a pure unsigned int
      // The easiest and least ugly way to get around is to cast to long explicitly
      ESP_LOGI (TAG, "Retry %d (mem:%ld)", handle->backoff, (long) esp_get_free_heap_size ());
      usleep (100000LL << handle->backoff);
   }
   handle_free (handle);
   vTaskDelete (NULL);
}

#ifdef	CONFIG_REVK_MQTT_SERVER
static void
server_task (void *pvParameters)
{
   lwmqtt_t handle = pvParameters;
   lwmqtt_loop (handle);
   handle_free (handle);
   vTaskDelete (NULL);
}

static void
listen_task (void *pvParameters)
{
   lwmqtt_t handle = pvParameters;
   if (handle)
   {
      struct sockaddr_in dst = {        // Yep IPv4 local
         .sin_addr.s_addr = htonl (INADDR_ANY),
         .sin_family = AF_INET,
         .sin_port = htons (handle->port),
      };
      int sock = socket (AF_INET, SOCK_STREAM, IPPROTO_IP);
      if (sock >= 0)
      {
         if (bind (sock, (void *) &dst, sizeof (dst)) < 0 || listen (sock, 1) < 0)
            close (sock);
         else
         {
            ESP_LOGD (TAG, "Listening for MQTT on %d", handle->port);
            while (handle->running)
            {                   // Loop connecting and trying repeatedly
               struct sockaddr_in addr;
               socklen_t addrlen = sizeof (addr);
               int s = accept (sock, (void *) &addr, &addrlen);
               if (s < 0)
                  break;
               ESP_LOGD (TAG, "Connect on MQTT %d", handle->port);
               lwmqtt_t h = mallocspi (sizeof (*h));
               if (!h)
                  break;
               memset (h, 0, sizeof (*h));
               h->port = handle->port;  // Only for debugging
               h->callback = handle->callback;
               h->arg = h;
               h->mutex = xSemaphoreCreateBinary ();
               h->server = 1;
               h->sock = s;
               h->running = 1;
               if (handle->ca_cert_bytes)
               {                // TLS
#ifdef CONFIG_ESP_TLS_SERVER
                  esp_tls_cfg_server_t cfg = {
                     .cacert_buf = handle->ca_cert_buf,
                     .cacert_bytes = handle->ca_cert_bytes,
                     .servercert_buf = handle->our_cert_buf,
                     .servercert_bytes = handle->our_cert_bytes,
                     .serverkey_buf = handle->our_key_buf,
                     .serverkey_bytes = handle->our_key_bytes,
                  };
                  h->tls = esp_tls_init ();
                  esp_err_t e = 0;
                  if (!h->tls || (e = esp_tls_server_session_create (&cfg, s, h->tls)))
                  {
                     ESP_LOGE (TAG, "TLS server failed %s", h->tls ? esp_err_to_name (e) : "No TLS");
                     h->running = 0;
                  } else
                  {             // Check client name? Do login callback
                     // TODO server client name check
                  }
#else
                  ESP_LOGE (TAG, "Not built for TLS server");
                  h->running = 0;
#endif
               }
               if (h->running)
               {
                  TaskHandle_t task_id = NULL;
                  xTaskCreate (server_task, "mqtt-server", 5 * 1024, (void *) h, 2, &task_id);
               } else
               {                // Close
                  ESP_LOGI (TAG, "MQTT aborted");
#ifdef CONFIG_ESP_TLS_SERVER
                  if (h->tls)
                     esp_tls_server_session_delete (h->tls);
#endif
                  close (h->sock);
               }

            }
         }
      }
      handle_free (handle);
   }
   vTaskDelete (NULL);
}
#endif

// Simple send - non retained no wait topic ends on space then payload
const char *
lwmqtt_send_str (lwmqtt_t handle, const char *msg)
{
   if (!handle || !*msg)
      return NULL;
   const char *p;
   for (p = msg; *p && *p != '\t'; p++);
   if (!*p)
      for (p = msg; *p && *p != ' '; p++);
   int tlen = p - msg;
   if (*p)
      p++;
   return lwmqtt_send_full (handle, tlen, msg, strlen (p), (void *) p, 0);
}

uint32_t
lwmqtt_connected (lwmqtt_t handle)
{                               // Confirm connected (and for how long)
   if (!handle || !handle->connected)
      return 0;
   return (uptime () - handle->connecttime) ? : 1;
}

int
lwmqtt_failed (lwmqtt_t handle)
{                               // Confirm failed
   if (!handle)
      return 255;               // non existent
   if (handle->connected)
      return 0;                 // Actually connected
   if (handle->failed)
      return -handle->failed;   // failed
   return handle->backoff;      // Trying
}
