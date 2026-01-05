// LED strip driver (SPI)

#include "revk.h"
#ifdef  CONFIG_REVK_LED
#include <stdarg.h>
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "soc/spi_periph.h"
#include <driver/gpio.h>
#include "esp_log.h"
#include "hal/spi_ll.h"

static const char __attribute__((unused)) TAG[] = "LED";

#ifdef	CONFIG_REVK_LED_FULL
static const uint8_t bits[LED_TYPES] = {        //
   [LED_WS2812] = 3,            //
   [LED_SK6812] = 4,            //
   [LED_XINGLIGHT] = 4,         //
};
#endif

static const uint8_t orders[6][3] = {
   [LED_GRB] = {1, 0, 2},       //
   [LED_GBR] = {2, 0, 1},       //
   [LED_RGB] = {0, 1, 2},       //
   [LED_RBG] = {0, 2, 1},       //
   [LED_BGR] = {2, 1, 0},       //
   [LED_BRG] = {1, 2, 0},       //
};

typedef struct led_channel_s *led_channel_t;

struct led_channel_s
{
   led_channel_t next;          // Next channel
   uint8_t gpio;                // GPIO
   uint8_t invert:1;            // GPIO invert
#ifdef	CONFIG_REVK_LED_FULL
   uint8_t bits:3;              // Bits per bit (based on type)
#endif
   uint8_t *mem;                // Allocated memory for strip DMA
   uint16_t size;               // Size of allocated memory
};

#define	LED_RESET	0       // Reset (may not be needed)
#define SPI_MAX		4096

struct led_strip_s
{
   led_strip_t next;            // Next strip
   led_channel_t channel;       // Channel
   uint16_t leds;               // Number of LEDs
   uint16_t size;               // Number of bytes
#ifdef	CONFIG_REVK_LED_FULL
   uint8_t type:2;              // Strip type
#endif
   uint8_t colours:3;           // Number of colours per LED
   uint8_t map[7];              // Map of RGB... to colour in memory (based on order)
   uint16_t offset;             // Offset in to allocated memory in channel
};

static led_channel_t channel = NULL;
static led_strip_t strip = NULL;
static spi_host_device_t led_spi = SPI_HOST_MAX;
static spi_device_handle_t handle = NULL;

// Functions return NULL if OK, else error message

static void
deinit (void)
{                               // Free stuff
   while (strip)
   {
      led_strip_t s = strip;
      strip = s->next;
      free (s);
   }
   while (channel)
   {
      led_channel_t c = channel;
      channel = c->next;
      heap_caps_free (c->mem);
      free (c);
   }
}

static const char *
set_spi (spi_host_device_t host)
{                               // Claim/release SPI
   if (led_spi == host)
      return NULL;
   if (host == SPI_HOST_MAX)
   {                            // Release SPI
      if (handle)
      {
         spi_bus_remove_device (handle);
         handle = NULL;
      }
      spi_bus_free (led_spi);
      led_spi = SPI_HOST_MAX;
   }
   if (led_spi == host)
      return NULL;
   // Claim SPI
   spi_bus_config_t config = {
      .mosi_io_num = -1,        // WE DO NOT SET GPIO HERE
      .miso_io_num = -1,
      .sclk_io_num = -1,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .data_io_default_level = 0,       // low for end of message reset and idle
      .max_transfer_sz = SPI_LL_DMA_MAX_BIT_LEN / 8,
   };
   esp_err_t e = spi_bus_initialize (host, &config, SPI_DMA_CH_AUTO);
   if (e)
      return esp_err_to_name (e);
   spi_device_interface_config_t device = {
      .clock_speed_hz = 1000000,
      .spics_io_num = -1,
      .queue_size = 4,
   };
   e = spi_bus_add_device (host, &device, &handle);
   if (e)
   {
      spi_bus_free (led_spi);
      return esp_err_to_name (e);
   }
   led_spi = host;
   return NULL;
}

// This claims the specified spi host - optional, this is called on first led_strip with a default SPI host otherwise
// It also clears / releases any previous resources
// THIS INVALIDATES EXISTING STRIP HANDLES - DO NOT CALL DURING led_send()
const char *
led_init (int spihost)
{
   deinit ();
   return set_spi (spihost);
}

// This releases the SPI host and frees resources
// THIS INVALIDATES EXISTING STRIP HANDLES - DO NOT CALL DURING led_send()
const char *
led_deinit (void)
{
   deinit ();
   return set_spi (SPI_HOST_MAX);
}

// This adds a new strip. If multiple strips on the same GPIO, add in order. Sets *strip
// Do not call during led_send().
const char *
led_strip (led_strip_t *stripp, // Where to store strip handle (stores NULL if error)
           uint8_t gpio,        // GPIO
           uint8_t invert,      // GPIO invert
#ifdef	CONFIG_REVK_LED_FULL
           uint8_t type,        // Strip type (for timing)
#endif
           uint16_t leds,       // Number of LEDs
           uint8_t colours,     // Number of colours, normally 3 or 4, but can be more
           uint8_t order        // RGB Colour order
   )
{
   if (led_spi == SPI_HOST_MAX)
      set_spi (SPI3_HOST);
   if (!stripp)
      return "No strip pointer";
   *stripp = NULL;
   if (!colours)
      return "No colours";
#ifdef	CONFIG_REVK_LED_FULL
   if (type >= LED_TYPES)
      return "Invalid type";
#endif
   if (order >= 6)
      return "Invalid order";
   led_channel_t c = channel;
   while (c && c->gpio != gpio)
      c = c->next;
   if (c)
   {
      if (c->invert != invert)
         return "Invert mismatch";
#ifdef	CONFIG_REVK_LED_FULL
      if (c->bits != bits[type])
         return "Type mismatch";
#endif
   } else
   {
      if (gpio_reset_pin (gpio) || gpio_set_level (gpio, 0) || gpio_set_direction (gpio, GPIO_MODE_OUTPUT))
         return "GPIO error";
      c = heap_caps_malloc_prefer (sizeof (*c), MALLOC_CAP_SPIRAM);
      if (!c)
         return "malloc";
      memset (c, 0, sizeof (*c));
      c->gpio = gpio;
#ifdef	CONFIG_REVK_LED_FULL
      c->bits = bits[type];
#endif
      c->invert = invert;
      c->next = channel;
      channel = c;
   }
   uint16_t base = c->size;
#ifdef	CONFIG_REVK_LED_FULL
   uint32_t size = (uint32_t) c->bits * colours * leds;
#else
   uint32_t size = (uint32_t) 4 * colours * leds;
#endif
   uint32_t new = (uint32_t) base + size;
   if (LED_RESET + new > SPI_LL_DMA_MAX_BIT_LEN / 8)
      return "Too many LEDs";   // Keep it sensible
   uint8_t *mem = heap_caps_realloc (c->mem, LED_RESET + new, MALLOC_CAP_DMA);
   if (!mem)
      return "malloc";
   memset (mem, 0, LED_RESET);
   c->mem = mem;
   c->size = new;
   led_strip_t s = heap_caps_malloc_prefer (sizeof (*s), MALLOC_CAP_SPIRAM);
   if (!s)
      return "malloc";
   memset (s, 0, sizeof (*s));
   s->channel = c;
#ifdef	CONFIG_REVK_LED_FULL
   s->type = type;
#endif
   s->leds = leds;
   s->size = size;
   s->colours = colours;
   s->map[0] = orders[order][0];
   s->map[1] = orders[order][1];
   s->map[2] = orders[order][2];
   for (int i = 3; i < colours; i++)
      s->map[i] = i;
   s->offset = LED_RESET + base;
   s->next = strip;
   strip = s;
   *stripp = s;
   led_clear (s);
   return NULL;
}

// Clear strip (all LEDs off)
const char *
led_clear (led_strip_t s)
{
   if (!s)
      return "No strip";
   uint8_t *m = s->channel->mem + s->offset;
   uint8_t *e = m + s->size;
#ifdef	CONFIG_REVK_LED_FULL
   if (s->channel->bits == 3)
      while (m < e)
      {
         *m++ = 0x92;           // 0 bits
         *m++ = 0x49;
         *m++ = 0x24;
      }                         //
   else if (s->channel->bits == 4)
#endif
      while (m < e)
         *m++ = 0x88;           // 0 bits
   return NULL;
}

// Set value of LED, called with three uint8_t for R/G/B, or more if more than 3 colours on strip, e.g. R/G/B/W
const char *
led_set (led_strip_t s, uint16_t led, ...)
{
   if (!s)
      return "No strip";
   if (led >= s->leds)
      return "Off end of strip";
   va_list ap;
   va_start (ap, led);
   for (int c = 0; c < s->colours; c++)
   {
      uint8_t v = va_arg (ap, int);
#ifdef	CONFIG_REVK_LED_FULL
      uint8_t *m = s->channel->mem + s->offset + ((uint32_t) led * s->colours + s->map[c]) * s->channel->bits;
      switch (s->type)
      {
      case LED_WS2812:
         *m++ = 0x92 + ((v & 0x80) >> 1) + ((v & 0x40) >> 3) + ((v & 0x20) >> 5);
         *m++ = 0x49 + ((v & 0x10) << 1) + ((v & 0x08) >> 1);
         *m++ = 0x24 + ((v & 0x04) << 5) + ((v & 0x02) << 3) + ((v & 0x01) << 1);
         break;
      case LED_SK6812:
         *m++ = ((v & 0x80 ? 0xC0 : 0x80) | (v & 0x40 ? 0x0C : 0x08));
         *m++ = ((v & 0x20 ? 0xC0 : 0x80) | (v & 0x10 ? 0x0C : 0x08));
         *m++ = ((v & 0x08 ? 0xC0 : 0x80) | (v & 0x04 ? 0x0C : 0x08));
         *m++ = ((v & 0x02 ? 0xC0 : 0x80) | (v & 0x01 ? 0x0C : 0x08));
         break;
      case LED_XINGLIGHT:
         *m++ = ((v & 0x80 ? 0xE0 : 0x80) | (v & 0x40 ? 0x0E : 0x08));
         *m++ = ((v & 0x20 ? 0xE0 : 0x80) | (v & 0x10 ? 0x0E : 0x08));
         *m++ = ((v & 0x08 ? 0xE0 : 0x80) | (v & 0x04 ? 0x0E : 0x08));
         *m++ = ((v & 0x02 ? 0xE0 : 0x80) | (v & 0x01 ? 0x0E : 0x08));
         break;
      }
#else
      // Simplified
      uint8_t *m = s->channel->mem + s->offset + ((uint32_t) led * s->colours + s->map[c]) * 4;
      *m++ = ((v & 0x80 ? 0xE0 : 0x80) | (v & 0x40 ? 0x0E : 0x08));
      *m++ = ((v & 0x20 ? 0xE0 : 0x80) | (v & 0x10 ? 0x0E : 0x08));
      *m++ = ((v & 0x08 ? 0xE0 : 0x80) | (v & 0x04 ? 0x0E : 0x08));
      *m++ = ((v & 0x02 ? 0xE0 : 0x80) | (v & 0x01 ? 0x0E : 0x08));
#endif
   }
   va_end (ap);
   return NULL;
}

// This updates all strips
const char *
led_send (void)
{
   if (!channel)
      return "No strips";
   led_channel_t c = channel;
   while (c)
   {
      spi_transaction_t txn = {
         .length = 8 * (LED_RESET + c->size),
         .tx_buffer = c->mem,
#ifdef	CONFIG_REVK_LED_FULL
         .override_freq_hz = 2500000 * c->bits / 3,
#else
         .override_freq_hz = 2500000 * 4 / 3,
#endif
      };
      esp_rom_gpio_connect_out_signal (c->gpio, spi_periph_signal[led_spi].spid_out, c->invert, false);
      esp_err_t e = spi_device_transmit (handle, &txn);
      esp_rom_gpio_connect_out_signal (c->gpio, SIG_GPIO_OUT_IDX, c->invert, false);

      if (e)
         return esp_err_to_name (e);
      c = c->next;
   }
   return NULL;
}

#endif
