// (new) settings library
#include "revk.h"
#ifndef  CONFIG_REVK_OLD_SETTINGS
static const char __attribute__((unused)) * TAG = "Settings";

extern revk_settings_t revk_settings[];
extern uint32_t revk_nvs_time;

static nvs_handle nvs[2] = { -1, -1 };

static revk_setting_bits_t nvs_found = { 0 };

char *__malloc_like __result_use_check
strdup (const char *s)
{
   int l = strlen (s);
   char *o = mallocspi (l + 1);
   if (!o)
      return NULL;
   memcpy (o, s, l + 1);
   return o;
}

char *__malloc_like __result_use_check
strndup (const char *s, size_t l)
{
   int l2 = strlen (s);
   if (l2 < l)
      l = l2;
   char *o = mallocspi (l + 1);
   if (!o)
      return NULL;
   memcpy (o, s, l);
   o[l] = 0;
   return o;
}

static const char *
nvs_erase (revk_settings_t * s, const char *tag)
{
   nvs_found[(s - revk_settings) / 8] &= ~(1 << ((s - revk_settings) & 7));
   esp_err_t e = nvs_erase_key (nvs[s->revk], tag);
   if (e && e != ESP_ERR_NVS_NOT_FOUND)
      return "Failed to erase";
   if (!e)
   {
      revk_nvs_time = uptime () + 60;
#ifdef  CONFIG_REVK_SETTINGS_DEBUG
      char taga[20];
      {
         int l = strlen (tag);
         if (tag[l - 1] & 0x80)
            sprintf (taga, "%.*s[%d]", l - 1, tag, tag[l - 1] - 0x80);
         else
            strcpy (taga, tag);
      }
      ESP_LOGE (TAG, "Erase %s", taga);
#endif
   }
   return NULL;
}

static const char *
nvs_put (revk_settings_t * s, int index, void *ptr)
{                               // Put data, can be from ptr or from settings
   if (s->array && index >= s->array)
      return "Array overflow";
   char tag[16];
   if (s->len + 1 + (s->array ? 1 : 0) > sizeof (tag))
      return "Tag too long";
#ifdef  CONFIG_REVK_SETTINGS_DEBUG
   char taga[20];
   if (s->array)
      sprintf (taga, "%s[%d]", s->name, index);
   else
      strcpy (taga, s->name);
#endif
   strcpy (tag, s->name);
   if (s->array)
   {
      tag[s->len] = 0x80 + index;
      tag[s->len + 1] = 0;
   }
   // Deference
   if (ptr)
   {                            // Copy
      if (s->malloc)
         ptr = *(void **) ptr;
   } else if (!ptr && s->ptr)
   {                            // Live (not relevant for bit as no s->ptr)
      if (s->malloc)
         ptr = ((void **) s->ptr)[index];
      else
         ptr = s->ptr + index * s->size;
   }
   revk_nvs_time = uptime () + 60;
   nvs_found[(s - revk_settings) / 8] |= (1 << ((s - revk_settings) & 7));
   switch (s->type)
   {
#ifdef	REVK_SETTINGS_HAS_SIGNED
   case REVK_SETTINGS_SIGNED:
      {
         int64_t __attribute__((unused)) v = 0;
         if ((s->size == 8 && nvs_set_i64 (nvs[s->revk], tag, v = *((int64_t *) ptr))) ||       //
             (s->size == 4 && nvs_set_i32 (nvs[s->revk], tag, v = *((int32_t *) ptr))) ||       //
             (s->size == 2 && nvs_set_i16 (nvs[s->revk], tag, v = *((int16_t *) ptr))) ||       //
             (s->size == 1 && nvs_set_i8 (nvs[s->revk], tag, v = *((int8_t *) ptr))))
            return "Cannot store number (signed)";
#ifdef  CONFIG_REVK_SETTINGS_DEBUG
         ESP_LOGE (TAG, "Write %s signed %lld 0x%0*llX", taga, v, s->size * 2, v);
#endif
      }
      break;
#endif
#ifdef	REVK_SETTINGS_HAS_UNSIGNED
   case REVK_SETTINGS_UNSIGNED:
      {
         uint64_t __attribute__((unused)) v = 0;
         if ((s->size == 8 && nvs_set_u64 (nvs[s->revk], tag, v = *((uint64_t *) ptr))) ||      //
             (s->size == 4 && nvs_set_u32 (nvs[s->revk], tag, v = *((uint32_t *) ptr))) ||      //
             (s->size == 2 && nvs_set_u16 (nvs[s->revk], tag, v = *((uint16_t *) ptr))) ||      //
             (s->size == 1 && nvs_set_u8 (nvs[s->revk], tag, v = *((uint8_t *) ptr))))
            return "Cannot store number (unsigned)";
#ifdef	CONFIG_REVK_SETTINGS_DEBUG
         ESP_LOGE (TAG, "Write %s unsigned %llu 0x%0*llX", taga, v, s->size * 2, v);
#endif
      }
      break;
#endif
#ifdef	REVK_SETTINGS_HAS_BIT
   case REVK_SETTINGS_BIT:
      {
         uint8_t bit = 0;
         if (ptr)
            bit = *((uint8_t *) ptr);
         else
            bit = ((((uint8_t *) & revk_settings_bits)[s->bit / 8] & (1 << (s->bit & 7))) ? 1 : 0);
         if (nvs_set_u8 (nvs[s->revk], tag, bit))
            return "Cannot store bit";
#ifdef  CONFIG_REVK_SETTINGS_DEBUG
         ESP_LOGE (TAG, "Write %s bit %d", taga, bit);
#endif
      }
      break;
#endif
#ifdef	REVK_SETTINGS_HAS_BLOB
   case REVK_SETTINGS_BLOB:
      {
         revk_settings_blob_t *b = ptr;
         if (nvs_set_blob (nvs[s->revk], tag, b->data, b->len))
            return "Cannot store blob";
#ifdef  CONFIG_REVK_SETTINGS_DEBUG
         ESP_LOGE (TAG, "Write %s fixed %d", taga, b->len);
         ESP_LOG_BUFFER_HEX_LEVEL (TAG, b->data, b->len, ESP_LOG_ERROR);
#endif
      }
      break;
#endif
#ifdef	REVK_SETTINGS_HAS_STRING
   case REVK_SETTINGS_STRING:
      {
         if (nvs_set_str (nvs[s->revk], tag, ptr))
            return "Cannot store string";
#ifdef  CONFIG_REVK_SETTINGS_DEBUG
         ESP_LOGE (TAG, "Write %s string %s", taga, (char *) ptr);
#endif
      }
      break;
#endif
#ifdef	REVK_SETTINGS_HAS_OCTET
   case REVK_SETTINGS_OCTET:
      {
         if (nvs_set_blob (nvs[s->revk], tag, ptr, s->size))
            return "Cannot store octets";
#ifdef  CONFIG_REVK_SETTINGS_DEBUG
         ESP_LOGE (TAG, "Write %s fixed %d", taga, s->size);
         ESP_LOG_BUFFER_HEX_LEVEL (TAG, ptr, s->array, ESP_LOG_ERROR);
#endif
      }
      break;
#endif
   }
   return NULL;
}

static const char *
nvs_get (revk_settings_t * s, const char *tag, int index)
{                               // Getting NVS
   if (s->array && index >= s->array)
      return "Array overflow";
#ifdef	CONFIG_REVK_SETTINGS_DEBUG
   char taga[20];
   {
      int l = strlen (tag);
      if (tag[l - 1] & 0x80)
         sprintf (taga, "%.*s[%d]", l - 1, tag, tag[l - 1] - 0x80);
      else
         strcpy (taga, tag);
   }
#endif
   void *data = NULL;
   size_t len = 0;
   const char *store (void)
   {
      switch (s->type)
      {
#ifdef  REVK_SETTINGS_HAS_SIGNED
      case REVK_SETTINGS_SIGNED:
         {
            data = mallocspi (len = s->size);
            if (!data)
               return "malloc";
            if ((s->size == 8 && nvs_get_i64 (nvs[s->revk], tag, data)) ||      //
                (s->size == 4 && nvs_get_i32 (nvs[s->revk], tag, data)) ||      //
                (s->size == 2 && nvs_get_i16 (nvs[s->revk], tag, data)) ||      //
                (s->size == 1 && nvs_get_i8 (nvs[s->revk], tag, data)))
               return "Cannot load number (signed)";
#ifdef	CONFIG_REVK_SETTINGS_DEBUG
            int64_t v = (int64_t) (s->size == 1 ? *((int8_t *) data) : s->size == 2 ? *((int16_t *) data) : s->size ==
                                   +4 ? *((int32_t *) data) : *((int64_t *) data));
            ESP_LOGE (TAG, "Read %s signed %lld 0x%0*llX", taga, v, s->size * 2, v);
#endif
         }
         break;
#endif
#ifdef  REVK_SETTINGS_HAS_UNSIGNED
      case REVK_SETTINGS_UNSIGNED:
         {
            data = mallocspi (len = s->size);
            if (!data)
               return "malloc";
            if ((s->size == 8 && nvs_get_u64 (nvs[s->revk], tag, data)) ||      //
                (s->size == 4 && nvs_get_u32 (nvs[s->revk], tag, data)) ||      //
                (s->size == 2 && nvs_get_u16 (nvs[s->revk], tag, data)) ||      //
                (s->size == 1 && nvs_get_u8 (nvs[s->revk], tag, data)))
               return "Cannot load number (unsigned)";
#ifdef	CONFIG_REVK_SETTINGS_DEBUG
            uint64_t v = (uint64_t) (s->size == 1 ? *((uint8_t *) data) : s->size == 2 ? *((uint16_t *) data) : s->size ==
                                     +4 ? *((uint32_t *) data) : *((uint64_t *) data));
            ESP_LOGE (TAG, "Read %s unsigned %llu 0x%0*llX", taga, v, s->size * 2, v);
#endif
         }
         break;
#endif
#ifdef  REVK_SETTINGS_HAS_BIT
      case REVK_SETTINGS_BIT:
         {
            data = mallocspi (len = 1);
            if (nvs_get_u8 (nvs[s->revk], tag, data))
               return "Cannot load bit";
#ifdef	CONFIG_REVK_SETTINGS_DEBUG
            ESP_LOGE (TAG, "Read %s bit %d", taga, *(uint8_t *) data);
#endif
         }
         break;
#endif
#ifdef  REVK_SETTINGS_HAS_BLOB
      case REVK_SETTINGS_BLOB:
         {
            if (nvs_get_blob (nvs[s->revk], tag, NULL, &len))
               return "Cannot get blob len";
            revk_settings_blob_t *b = mallocspi (sizeof (revk_settings_blob_t) + len);
            if (!b)
               return "malloc";
            b->len = len;
            if (nvs_get_blob (nvs[s->revk], tag, b->data, &len))
               return "Cannot load blob";
#ifdef	CONFIG_REVK_SETTINGS_DEBUG
            ESP_LOGE (TAG, "Read %s blog %d", taga, b->len);
            ESP_LOG_BUFFER_HEX_LEVEL (TAG, b->data, b->len, ESP_LOG_ERROR);
#endif
            data = b;
         }
         break;
#endif
#ifdef  REVK_SETTINGS_HAS_STRING
      case REVK_SETTINGS_STRING:
         {
            if (nvs_get_str (nvs[s->revk], tag, NULL, &len))
               return "Cannot get string len";
            if (!len)
               return "Bad string len";
            data = mallocspi (len);
            if (!data)
               return "malloc";
            if (nvs_get_str (nvs[s->revk], tag, data, &len))
               return "Cannot load string";
            ((char *) data)[len - 1] = 0;       // Just in case
#ifdef	CONFIG_REVK_SETTINGS_DEBUG
            ESP_LOGE (TAG, "Read %s string %s", taga, (char *) data);
#endif
         } break;
#endif
#ifdef  REVK_SETTINGS_HAS_OCTET
      case REVK_SETTINGS_OCTET:
         {
            data = mallocspi (len = s->size);
            if (!data)
               return "malloc";
            if (nvs_get_blob (nvs[s->revk], tag, data, &len))
               return "Cannot load fixed block";
            if (len != s->size)
               return "Bad fixed block size";
#ifdef	CONFIG_REVK_SETTINGS_DEBUG
            ESP_LOGE (TAG, "Read %s fixed %d", taga, len);
            ESP_LOG_BUFFER_HEX_LEVEL (TAG, data, len, ESP_LOG_ERROR);
#endif
         }
         break;
#endif
      }
      return NULL;
   }
   const char *err = store ();
   if (err)
   {
      free (data);
      return err;
   }
   if (s->malloc)
   {
      void **p = s->ptr;
      p += index;
      free (*p);
      *p = data;
   }
#ifdef  REVK_SETTINGS_HAS_BIT
   else if (s->type == REVK_SETTINGS_BIT)
   {
      if (*(uint8_t *) data)
         ((uint8_t *) & revk_settings_bits)[s->bit / 8] |= (1 << (s->bit & 7));
      else
         ((uint8_t *) & revk_settings_bits)[s->bit / 8] &= ~(1 << (s->bit & 7));
      free (data);
   }
#endif
   else
   {
      if (len > s->size)
         err = "Bad size";
      else
         memcpy (s->ptr + index * s->size, data, s->size);
      free (data);
   }
   return err;
}

#ifdef	REVK_SETTINGS_HAS_NUMERIC
static const char *
parse_numeric (revk_settings_t * s, void **pp, const char **dp, const char *e)
{                               // Single numeric parse to memory, advance memory and source
   if (!s || !dp || !pp)
      return "NULL";
   if (!s->size)
      return "Not numeric";
   const char *err = NULL;
   const char *d = *dp;
   void *p = *pp;
   while (d && d < e && *d == ' ')
      d++;
   if (!d || d >= e)
      memset (p, 0, s->size);   // Empty
   else
   {                            // Value
      uint64_t v = 0,
         f = 0;
      char sign = 0;
      int bits = s->size * 8;
      if (s->set)
         f |= (1ULL << (--bits));
      int top = bits - 1;
      const char *b = s->flags;
      void scan (void)
      {                         // Scan for flags
         while (d < e && *d != ' ' && *d != ',')
         {
            int l = 1;
            while (d + l < e && (d[l] & 0xC0) == 0x80)
               l++;
            int bit = top;
            const char *q;
            for (q = b; *q; q++)
               if (!(f & (1ULL << bit)) && !memcmp (q, d, l))   // Allows for duplicates in flags
               {
                  f |= (1ULL << bit);
                  d += l;
                  break;
               } else if (*q != ' ' && ((*q & 0xC0) != 0x80))
                  bit--;
            if (!*q)
               break;           // Not found
         }
      }
      if (b)
      {                         // Bit fields
         for (const char *q = b; *q; q++)
            if (*q != ' ' && ((*q & 0xC0) != 0x80))
               bits--;
         if (!err && bits < 0)
            err = "Too many flags";
         scan ();
      }
      // Numeric value
      int dig (int c)
      {
         if (c >= '0' && c <= '9')
            return c - '0';
         if (s->hex && ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
            return 9 + (c & 0xF);
         return -1;
      }
      void add (char c)
      {
         uint64_t wrap = v;
         v = v * (s->hex ? 16 : 10) + dig (c);
         if (!err && v < wrap)
            err = "Number too large";
      }
      if (d < e && *d == '-')
         sign = *d++;
      if (!err && bits && (d >= e || dig (*d) < 0))
         err = "No number found";
      while (!err && d < e && dig (*d) >= 0)
         add (*d++);
      if (!err && s->decimal)
      {
         int q = s->decimal;
         if (d < e && *d == '.')
         {
            d++;
            while (!err && d < e && dig (*d) >= 0 && q && q--)
               add (*d++);
            if (!err && d < e && dig (*d) >= 0)
               err = "Too many decimal places";
         }
         while (!err && q--)
            add ('0');
      }
      if (b)
         scan ();
      if (!err && sign
#ifdef	REVK_SETTINGS_HAS_UNSIGNED
          && s->type == REVK_SETTINGS_UNSIGNED
#endif
         )
         err = "Negative not allowed";
      if (!err && bits < 64 && v >= (1ULL << bits))
         err = "Number too big";
      if (sign)
         v = -v;
      if (bits < 64)
         v = ((v & ((1ULL << bits) - 1)) | f);
      if (!err)
      {
         if (s->size == 1)
            *((uint8_t *) p) = v;
         else if (s->size == 2)
            *((uint16_t *) p) = v;
         else if (s->size == 4)
            *((uint32_t *) p) = v;
         else if (s->size == 8)
            *((uint64_t *) p) = v;
         else
            err = "Bad number size";
      }
   }

   while (d && d < e && *d == ' ' && *d != ',' && *d != '\t')
      d++;
   if (d && d < e && (*d == ',' || *d == '\t'))
      d++;
   while (d && d < e && *d == ' ')
      d++;
   p += (s->malloc ? sizeof (void *) : s->size);
   *pp = p;
   *dp = d;
   return err;
}
#endif

#ifdef	REVK_SETTINGS_HAS_NUMERIC
static char *
text_numeric (revk_settings_t * s, void *p)
{
   char *temp = mallocspi (100),
      *t = temp;
   uint64_t v = 0;
   if (s->size == 8)
      v = *((uint64_t *) p);
   else if (s->size == 4)
      v = *((uint32_t *) p);
   else if (s->size == 2)
      v = *((uint16_t *) p);
   else if (s->size == 1)
      v = *((uint8_t *) p);
   int bits = s->size * 8;
   if (!s->set || (v & (1ULL << --bits)))
   {
      const char *f = NULL;
      int bit = bits - 1;
      if (s->flags)
      {
         // Count down bits in use
         for (f = s->flags; *f; f++)
            if (*f != ' ' && ((*f & 0xC0) != 0x80))
               bits--;
         // Prefix
         for (f = s->flags; *f && *f != ' '; f++)
            if (((*f & 0xC0) != 0x80) && (v & (1ULL << (bit--))))
            {
               const char *i = f;
               *t++ = *i++;
               while ((*i & 0xC0) == 0x80)
                  *t++ = *i++;
            }
      }
      {
         uint64_t val = v;
         if (bits < 64)
            val &= ((1ULL << bits) - 1);
#ifdef	REVK_SETTINGS_HAS_SIGNED
         if (s->type == REVK_SETTINGS_SIGNED && (v & (1ULL << (bits - 1))))
         {
            *t++ = '-';
            val = -val;
            if (bits < 64)
               val |= ~((1ULL << bits) - 1);
         }
#endif
         if (bits)
         {
            if (s->decimal)
            {
               sprintf (t, "%021llu", val);
               char *i = t,
                  *e = t + 21 - s->decimal;
               while (i < e - 1 && *i == '0')
                  i++;
               while (i < e)
                  *t++ = *i++;
               *t++ = '.';
               while (*i)
                  *t++ = *i++;
            } else if (s->hex)
               t += sprintf (t, "%llX", val);
            else
               t += sprintf (t, "%llu", val);
         }
      }
      // Suffix
      if (f && *f == ' ')
         for (f++; *f; f++)
            if (((*f & 0xC0) != 0x80) && (v & (1ULL << (bit--))))
            {
               const char *i = f;
               *t++ = *i++;
               while ((*i & 0xC0) == 0x80)
                  *t++ = *i++;
            }
   }
   *t = 0;
   return temp;
}
#endif

static int
value_cmp (revk_settings_t * s, void *a, void *b)
{                               // Pointer to actual data
   if (s->malloc)
   {
#ifdef	REVK_SETTINGS_HAS_BLOB
      if (s->type == REVK_SETTINGS_BLOB)
      {
         revk_settings_blob_t *A = *(void **) a;
         revk_settings_blob_t *B = *(void **) b;
         if (!A || !B || A->len > B->len)
            return 1;
         if (A->len < B->len)
            return -1;
         return memcmp (A->data, B->data, A->len);
      }
#endif
#ifdef	REVK_SETTINGS_HAS_STRING
      if (s->type == REVK_SETTINGS_STRING)
         return strcmp (*((char **) a), *((char **) b));
#endif
   }
   return memcmp (a, b, s->size ? : 1);
}

static char *
text_value (revk_settings_t * s, int index, int *lenp)
{                               // Malloc'd string for value
   void *ptr = s->ptr;
   if (s->array)
      ptr += index * (s->malloc ? sizeof (void *) : s->size);
   if (s->malloc)
      ptr = *(void **) ptr;
   char *data = NULL;
   size_t len = 0;
   switch (s->type)
   {
#ifdef	REVK_SETTINGS_HAS_NUMERIC
#ifdef	REVK_SETTINGS_HAS_SIGNED
   case REVK_SETTINGS_SIGNED:
#endif
#ifdef	REVK_SETTINGS_HAS_UNSIGNED
   case REVK_SETTINGS_UNSIGNED:
#endif
      data = text_numeric (s, ptr);
      if (data)
         len = strlen (data);
      break;
#endif
#ifdef	REVK_SETTINGS_HAS_BIT
   case REVK_SETTINGS_BIT:
      uint8_t bit = ((((uint8_t *) & revk_settings_bits)[s->bit / 8] & (1 << (s->bit & 7))) ? 1 : 0);
      data = strdup (bit ? "true" : "false");
      if (data)
         len = strlen (data);
      break;
#endif
#ifdef	REVK_SETTINGS_HAS_BLOB
   case REVK_SETTINGS_BLOB:
      {
         revk_settings_blob_t *b = ptr;
         len = b->len;
         if (len)
         {
            data = mallocspi (len);
            memcpy (data, b->data, len);
         }
      }
      break;
#endif
#ifdef	REVK_SETTINGS_HAS_STRING
   case REVK_SETTINGS_STRING:
      {
         data = strdup (ptr);
         if (data)
            len = strlen (data);
      }
      break;
#endif
#ifdef	REVK_SETTINGS_HAS_OCTET
   case REVK_SETTINGS_OCTET:
      {
         len = s->size;
         data = mallocspi (len);
         memcpy (data, ptr, len);
      }
      break;
#endif
   }
   if (lenp)
      *lenp = len;
   return data;
}

static const char *
load_value (revk_settings_t * s, const char *d, int index, void *ptr)
{
   if (!ptr)
      ptr = s->ptr;
   else
      index = 0;
   if (index > 0)
      ptr += index * (s->malloc ? sizeof (void *) : s->size);
   const char *err = NULL;
   int a = s->array;
   const char *e = NULL;
   char *mem = NULL;
   if (d)
   {
      e = d + strlen (d);
      if (s->dq && e > d + 1 && *d == '"' && e[-1] == '"')
      {
         d++;
         e--;
      }
      if (d == e)
         d = e = NULL;
   }
   if (d < e && (s->hex || s->base64)
#ifdef	REVK_SETTINGS_HAS_NUMERIC
#ifdef  REVK_SETTINGS_HAS_SIGNED
       && s->type != REVK_SETTINGS_SIGNED
#endif
#ifdef  REVK_SETTINGS_HAS_UNSIGNED
       && s->type != REVK_SETTINGS_UNSIGNED
#endif
#endif
      )
   {
      jo_t j = jo_create_alloc ();
      jo_stringn (j, NULL, d, e - d);
      jo_rewind (j);
      ssize_t len = jo_strncpyd (j, NULL, 0, s->base64 ? 6 : 4, s->base64 ? JO_BASE64 : JO_BASE16);
      if (len <= 0)
         err = s->base64 ? "Bad base64" : "Bad hex";
      if (err)
         d = e = NULL;
      else
      {
         mem = mallocspi (len);
         jo_strncpyd (j, mem, len, s->base64 ? 6 : 4, s->base64 ? JO_BASE64 : JO_BASE16);
         d = mem;
         e = mem + len;
      }
      jo_free (&j);
   }
   switch (s->type)
   {
#ifdef	REVK_SETTINGS_HAS_NUMERIC
#ifdef  REVK_SETTINGS_HAS_SIGNED
   case REVK_SETTINGS_SIGNED:
#endif
#ifdef  REVK_SETTINGS_HAS_UNSIGNED
   case REVK_SETTINGS_UNSIGNED:
#endif
      if (s->malloc)
         err = "Malloc number not supported";
      else
      {
         err = parse_numeric (s, &ptr, &d, e);
         if (a && index < 0)
            while (!err && --a)
               err = parse_numeric (s, &ptr, &d, e);
         if (!err && d < e)
            err = "Extra data on end of number";
      }
      break;
#endif
#ifdef  REVK_SETTINGS_HAS_BIT
   case REVK_SETTINGS_BIT:
      if (s->malloc)
         err = "Malloc bit not supported";
      else
      {
         if (ptr)
            *(uint8_t *) ptr = ((d < e && (*d == '1' || *d == 't')) ? 1 : 0);
         else
         {
            if (d < e && (*d == '1' || *d == 't'))
               ((uint8_t *) & revk_settings_bits)[s->bit / 8] |= (1 << (s->bit & 7));
            else
               ((uint8_t *) & revk_settings_bits)[s->bit / 8] &= ~(1 << (s->bit & 7));
         }
      }
      break;
#endif
#ifdef  REVK_SETTINGS_HAS_BLOB
   case REVK_SETTINGS_BLOB:
      if (!s->malloc)
         err = "Fixed blob not supported";
      else
      {
         void **p = (void **) ptr;
         if (d < e)
         {
            free (*p);
            *p = mallocspi (sizeof (revk_settings_blob_t) + e - d);
            ((revk_settings_blob_t *) (*p))->len = (e - d);
            memcpy ((*p) + sizeof (revk_settings_blob_t), d, e - d);
         } else
         {
            free (*p);
            *p = mallocspi (sizeof (revk_settings_blob_t));
            ((revk_settings_blob_t *) (*p))->len = 0;
         }
         if (a && index < 0)
            while (--a)
            {
               free (*p);
               *p = mallocspi (sizeof (revk_settings_blob_t));
               ((revk_settings_blob_t *) (*p))->len = 0;
               p++;
            }
      }
      break;
#endif
#ifdef  REVK_SETTINGS_HAS_STRING
   case REVK_SETTINGS_STRING:
      {
         if (!s->malloc)
         {                      // Fixed
            if (e - d + 1 > s->size)
               err = "Too long";
            else
            {
               memcpy (ptr, d, e - d);
               ((char *) ptr)[e - d] = 0;
            }
            if (a && index < 0)
               while (--a)
               {
                  memset (ptr, 0, s->size);
                  ptr += s->size;
               }
         } else
         {                      // Malloc
            void **p = (void **) ptr;
            free (*p);
            if (d)
               *p = strndup (d, (int) (e - d));
            else
               *p = strdup ("");
            if (a && index < 0)
               while (--a)
               {
                  free (*++p);
                  *p = strdup ("");
               }
         }
      }
      break;
#endif
#ifdef  REVK_SETTINGS_HAS_OCTET
   case REVK_SETTINGS_OCTET:
      if (s->malloc)
         err = "Malloc octet not supported";
      else
      {
         if (d < e)
         {
            if (e - d != s->size)
               err = "Wrong length";
            else
               memcpy (ptr, d, e - d);
         } else
            memset (ptr, 0, s->size);
         if (a && index < 0)
            while (--a)
            {
               memset (ptr, 0, s->size);
               ptr += s->size;
            }
      }
      break;
#endif
#ifdef  CONFIG_REVK_SETTINGS_DEBUG
   default:
      ESP_LOGE (TAG, "Bad type %s %d", s->name, s->type);
#endif
   }
   free (mem);
   return err;
}

void
revk_settings_load (const char *tag, const char *appname)
{                               // Scan NVS to load values to settings
   for (revk_settings_t * s = revk_settings; s->len; s++)
      load_value (s, s->def, -1, NULL);
   // Scan
   for (int revk = 0; revk < 2; revk++)
   {
      struct zap_s
      {
         struct zap_s *next;
         char tag[0];
      } *zap = NULL;
      nvs_open_from_partition (revk ? tag : "nvs", revk ? tag : appname, NVS_READWRITE, &nvs[revk]);
      nvs_iterator_t i = NULL;
      if (!nvs_entry_find (revk ? tag : "nvs", revk ? tag : appname, NVS_TYPE_ANY, &i))
      {
         do
         {
            nvs_entry_info_t info = { 0 };
            void addzap (void)
            {
               struct zap_s *z = malloc (sizeof (*z) + strlen (info.key) + 1);
               strcpy (z->tag, info.key);
               z->next = zap;
               zap = z;
            }
            if (!nvs_entry_info (i, &info))
            {
               int l = strlen (info.key);
               revk_settings_t *s;
               const char *err = NULL;
               for (s = revk_settings; s->len; s++)
               {
                  if (!s->array && s->len == l && !memcmp (s->name, info.key, l))
                  {             // Exact match
                     err = nvs_get (s, info.key, 0);
                     break;
                  } else if (s->array && s->len + 1 == l && !memcmp (s->name, info.key, s->len) && (info.key[s->len] & 0x80))
                  {             // Array match, new
                     err = nvs_get (s, info.key, info.key[s->len] - 0x80);
                     break;
                  } else if (s->array && s->len < l && !memcmp (s->name, info.key, s->len) && isdigit ((int) info.key[s->len]))
                  {             // Array match, old
                     err = nvs_get (s, info.key, atoi (info.key + s->len) - 1);
                     if (!err)
                        err = "Old style record in nvs, being replaced";        // And error should mean any .fix is written back
                     addzap ();
                     break;
                  }
               }
               if (!s->len)
               {
                  addzap ();
                  err = "Not found";
               }
               if (err)
                  ESP_LOGE (tag, "NVS %s Failed %s", info.key, err);
               else
                  nvs_found[(s - revk_settings) / 8] |= (1 << ((s - revk_settings) & 7));
            }
         }
         while (!nvs_entry_next (&i));
      }
      nvs_release_iterator (i);
      while (zap)
      {
         struct zap_s *z = zap->next;
         if (nvs_erase_key (nvs[revk], zap->tag))
            ESP_LOGE (tag, "Erase %s failed", zap->tag);
#ifdef  CONFIG_REVK_SETTINGS_DEBUG
         else
            ESP_LOGE (TAG, "Erase %s", zap->tag);
#endif
         free (zap);
         zap = z;
      }
   }
   for (revk_settings_t * s = revk_settings; s->len; s++)
      if (s->fix && !(nvs_found[(s - revk_settings) / 8] & (1 << ((s - revk_settings) & 7))))
      {                         // Fix, save to flash
         if (!s->array)
            nvs_put (s, 0, NULL);
         else
            for (int i = 0; i < s->array; i++)
               nvs_put (s, i, NULL);
      }
}

const char *
revk_setting_dump (int level)
{
   // level 1 - all stored
   // level 2 - all stored or with default
   // level 3 - all
   int is_zero (revk_settings_t * s, int index)
   {
#ifdef  REVK_SETTINGS_HAS_BIT
      if (s->type == REVK_SETTINGS_BIT && !(((uint8_t *) & revk_settings_bits)[s->bit / 8] & (1 << (s->bit & 7))))
         return 1;
#endif
      uint8_t *d = s->ptr;
      if (s->malloc)
      {
         void **p = s->ptr;
         p += index;
         d = (*p);
         if (!d)
            return 1;
      } else
         d += index * s->size;
      if (s->size)
      {
         int i;
         for (i = 0; i < s->size && !d[i]; i++);
         return i == s->size;
      }
      switch (s->type)
      {
#ifdef  REVK_SETTINGS_HAS_BLOB
      case REVK_SETTINGS_BLOB:
         return ((revk_settings_blob_t *) (d))->len == 0;
#endif
#ifdef  REVK_SETTINGS_HAS_STRING
      case REVK_SETTINGS_STRING:
         return *d == 0;
#endif
      }
      return 0;
   }
   int visible (revk_settings_t * s)
   {
      if (s->secret)
         return 0;
      if (level > 2)
         return 1;
      if (level <= 1 && !s->array && s->fix && (!s->def || !*s->def || (s->dq && !strcmp (s->def, "\"\""))) && is_zero (s, 0))
         return 0;
      if (nvs_found[(s - revk_settings) / 8] & (1 << ((s - revk_settings) & 7)))
         return 1;
      if (level > 1 && !s->array && !(!s->def || !*s->def || (s->dq && !strcmp (s->def, "\"\""))))
         return 1;
      return 0;
   }
   const char *err = NULL;
   jo_t j = NULL;
   void send (void)
   {                            // Sends the settings - this deliberately uses the revk_id not the hostname as it is "seen" by any device listening
      if (!j)
         return;
      const char *an = appname,
         *sl = "/";
      if (!prefixapp)
         an = sl = "";
      char *topic = NULL;
      asprintf (&topic, "%s%s%s/%s%s", prefixsetting, sl, an, revk_id, level <= 1 ? "" : "*");
      revk_mqtt_send (NULL, 0, topic, &j);
      free (topic);
   }
   int maxpacket = MQTT_MAX;
   maxpacket -= 50;             // for headers
#ifdef	CONFIG_REVK_MESH
   maxpacket -= MESH_PAD;
#endif
   char *buf = mallocspi (maxpacket);   // Allows for topic, header, etc
   if (!buf)
      return "malloc";
   revk_setting_group_t group = { 0 };
   for (revk_settings_t * s = revk_settings; s->len; s++)
   {
      if (s->group && (group[s->group / 8] & (1 << (s->group & 7))))
         continue;              // Already done
      jo_t p = NULL;
      void start (void)
      {
         if (!p)
         {
            if (j)
               p = jo_copy (j);
            else
            {
               p = jo_create_mem (buf, maxpacket);
               jo_object (p, NULL);
            }
         }
      }
      const char *failed (void)
      {
         err = NULL;
         if (p && (err = jo_error (p, NULL)))
            jo_free (&p);       // Did not fit
         return err;
      }

      void addvalue (revk_settings_t * s, int index, const char *tag)
      {
         if (!s->array && !visible (s))
            return;             // Default
         int len = 0;
         char *data = text_value (s, index, &len);
         switch (s->type)
         {
#ifdef  REVK_SETTINGS_HAS_BIT
         case REVK_SETTINGS_BIT:
            jo_lit (p, tag, data);
            break;
#endif
#ifdef  REVK_SETTINGS_HAS_NUMERIC
#ifdef  REVK_SETTINGS_HAS_SIGNED
         case REVK_SETTINGS_SIGNED:
#endif
#ifdef  REVK_SETTINGS_HAS_UNSIGNED
         case REVK_SETTINGS_UNSIGNED:
#endif
            if (len)
            {
               char *d = data;
               if (*d == '-')
                  d++;
               if (isdigit ((int) *d))
               {
                  while (isdigit ((int) *d))
                     d++;
                  if (*d == '.')
                  {
                     d++;
                     while (isdigit ((int) *d))
                        d++;
                  }
               }
               if (!*d && strcmp (data, "-0"))
               {
                  jo_lit (p, tag, data);
                  break;
               }
            }
            jo_stringn (p, tag, data ? : "", len);
            break;
#endif
         default:
            if (s->hex)
               jo_base16 (p, tag, data ? : "", len);
            else if (s->base64)
               jo_base64 (p, tag, data ? : "", len);
            else
               jo_stringn (p, tag, data ? : "", len);
         }
         free (data);
      }

      void addarray (revk_settings_t * s, int base)
      {
         if (!visible (s))
            return;             // Default
         int max = s->array;
         while (max > 0 && is_zero (s, max - 1))
            max--;
         if (max || level > 1 || !s->fix || !(!s->def || !*s->def || (s->dq && !strcmp (s->def, "\"\""))))
         {
            jo_array (p, s->name + base);
            for (int i = 0; i < max; i++)
               addvalue (s, i, NULL);
            jo_close (p);
         }
      }

      void addgroup (revk_settings_t * s)
      {
         group[s->group / 8] |= (1 << (s->group & 7));
         revk_settings_t *r;
         for (r = revk_settings; r->len && (r->group != s->group || !visible (r)); r++);
         if (!r->len)
            return;
         char tag[16];
         if (s->dot + 1 > sizeof (tag))
            return;
         strcpy (tag, s->name);
         tag[s->dot] = 0;
         jo_object (p, tag);
         for (r = revk_settings; r->len; r++)
            if (r->group == s->group && visible (r))
            {
               if (r->array)
                  addarray (r, r->dot);
               else
                  addvalue (r, 0, r->name + r->dot);
            }
         jo_close (p);
      }

      void addsetting (void)
      {                         // Add a whole setting
         start ();
         if (s->group)
            addgroup (s);
         else if (s->array)
            addarray (s, 0);
         else
            addvalue (s, 0, s->name);
      }

      addsetting ();
      if (failed () && j)
      {
         send ();               // Failed, clear what we were sending and try again
         addsetting ();
      }
      if (!failed ())
      {                         // Fitted, move forward
         if (p)
         {
            jo_free (&j);
            j = p;
         }
      } else
      {
         jo_t j = jo_make (NULL);
         jo_string (j, "description", "Setting did not fit");
         jo_string (j, prefixsetting, s->name);
         if (err)
            jo_string (j, "reason", err);
         revk_error (TAG, &j);
      }
   }
   send ();
   free (buf);
   return NULL;
}

const char *
revk_setting (jo_t j)
{
   if (!j)
      return "";
   jo_rewind (j);
   jo_type_t t;
   if ((t = jo_here (j)) != JO_OBJECT)
      return "Not an object";
   char change = 0;
   const char *err = NULL;
   char tag[16];
   revk_setting_bits_t found = { 0 };
   const char *debug = NULL;
   const char *scan (int plen, int pindex)
   {
      while (!err && (t = jo_next (j)) == JO_TAG)
      {
         debug = jo_debug (j);
         int l = jo_strlen (j);
         if (l + plen > sizeof (tag) - 1)
            return "Not found";
         jo_strncpy (j, tag + plen, l + 1);
         revk_settings_t *s;
         for (s = revk_settings; s->len && (s->len != plen + l || (plen && s->dot != plen) || strcmp (s->name, tag)); s++);
         const char *store (int index)
         {
            if (!s->len)
            {
               if (index < 0)
               {
                  int e = l;
                  while (e && isdigit ((int) tag[plen + e - 1]))
                     e--;
                  if (e < l)
                  {
                     for (s = revk_settings;
                          s->len && (!s->array || s->len != plen + e || (plen && s->dot != plen)
                                     || strncmp (s->name, tag, plen + e)); s++);
                     if (s)
                        index = atoi (tag + plen + e) - 1;
                  }
               }
               if (!s->len)
                  return "Not found";
            }
            if (index < 0)
               index = 0;
            found[(s - revk_settings) / 8] |= (1 << ((s - revk_settings) & 7));
            if ((s->array && (index < 0 || index >= s->array)) || (!s->array && index))
               return "Bad array index";
            char *val = NULL;
            if (t == JO_NULL)
            {                   // Default
               if (s->def)
               {
                  val = (char *) s->def;
#ifdef	REVK_SETTINGS_HAS_NUMERIC
                  if (s->array && (0
#ifdef	REVK_SETTINGS_HAS_SIGNED
                                   || s->type == REVK_SETTINGS_SIGNED
#endif
#ifdef	REVK_SETTINGS_HAS_UNSIGNED
                                   || s->type == REVK_SETTINGS_UNSIGNED
#endif
                      ))
                  {             // Skip to the entry
                     if (s->dq && *val == '"')
                        val++;
                     int i = index;
                     while (i--)
                     {
                        while (*val && *val != ',' && *val != ' ' && *val != '\t')
                           val++;
                        while (*val && *val == ' ')
                           val++;
                        if (*val && (*val == ',' || *val == '\t'))
                           val++;
                        while (*val && *val == ' ')
                           val++;
                     }
                  }
#endif
                  val = strdup (val);
#ifdef	REVK_SETTINGS_HAS_NUMERIC
                  if (s->array && (1
#ifdef	REVK_SETTINGS_HAS_SIGNED
                                   || s->type == REVK_SETTINGS_SIGNED
#endif
#ifdef	REVK_SETTINGS_HAS_UNSIGNED
                                   || s->type == REVK_SETTINGS_UNSIGNED
#endif
                      ))
                  {
                     char *v = val;
                     while (*v && *v != ' ' && *v != ',' && *v != '\t' && (s->dq && *v != '"'))
                        v++;
                     *v = 0;
                  }
#endif
               }
            } else if (t != JO_CLOSE)
               val = jo_strdup (j);
            int len = s->malloc ? sizeof (void *) : s->size ? : 1;
            uint8_t *temp = mallocspi (len);
            if (!temp)
               err = "malloc";
            else
            {
               memset (temp, 0, len);
               uint8_t dofree = s->malloc;
               uint8_t bit = 0;
               void *ptr = s->ptr ? : &bit;
#ifdef	REVK_SETTINGS_HAS_BIT
               if (s->type == REVK_SETTINGS_BIT)
                  bit = ((((uint8_t *) & revk_settings_bits)[s->bit / 8] & (1 << (s->bit & 7))) ? 1 : 0);
               else
#endif
                  ptr += index * (s->malloc ? sizeof (void *) : s->size);
               err = load_value (s, val, index, temp);
               if (!err)
               {
                  if (value_cmp (s, ptr, temp))
                  {             // Change
                     if (s->live)
                     {          // Apply live
#ifdef	REVK_SETTINGS_HAS_BIT
                        if (s->type == REVK_SETTINGS_BIT)
                        {
                           if (*temp)
                              ((uint8_t *) & revk_settings_bits)[s->bit / 8] |= (1 << (s->bit & 7));
                           else
                              ((uint8_t *) & revk_settings_bits)[s->bit / 8] &= ~(1 << (s->bit & 7));
                        } else
#endif
                        {
                           if (s->malloc)
                              free (*(void **) ptr);
                           memcpy (ptr, temp, len);
                           dofree = 0;
                        }
                     }
                     if (t == JO_NULL && !s->fix)
                        err = nvs_erase (s, s->name);
                     else
                        err = nvs_put (s, index, temp);
                     if (!err && !s->live)
                        change = 1;
                  } else if (t == JO_NULL && !s->fix)
                     err = nvs_erase (s, s->name);
               }
               if (dofree)
                  free (*(void **) temp);
               free (temp);
            }
            free (val);
            return err;
         }
         t = jo_next (j);
         void zapdef (void)
         {
            if (pindex >= 0)
               err = store (pindex);
            else if (s->array)
               for (int i = 0; !err && i < s->array; i++)
                  err = store (i);
            else
               err = store (-1);
         }
         if (t == JO_NULL)
         {
            if (s->len)
               zapdef ();
            else if (!plen)
            {
               for (s = revk_settings; s->len && (!s->group || s->dot != l || strncmp (s->name, tag, l)); s++);
               if (s->len)
               {                // object reset
                  int group = s->group;
                  for (s = revk_settings; s->len; s++)
                     if (s->group == group)
                        zapdef ();      // Including secrets
               }
            } else
               return "Invalid null";
         } else if (t == JO_OBJECT)
         {                      // Object
            if (plen)
               return "Nested too far";
            if (s->len)
               return "Not an object";
            for (s = revk_settings; s->len && (!s->group || s->dot != l || strncmp (s->name, tag, l)); s++);
            if (!s->len)
               err = "Unknown object";
            else
            {
               int group = s->group;
               err = scan (l, -1);
               if (err)
                  return err;
               t = JO_NULL;     // Set to default
               for (s = revk_settings; s->len; s++)
                  if (s->group == group && !(found[(s - revk_settings) / 8] & (1 << ((s - revk_settings) & 7))) && !s->secret)
                     zapdef (); // Not secrets, D'Oh
            }
         } else if (t == JO_ARRAY)
         {                      // Array
            if (pindex >= 0)
               return "Unexpected array";
            if (!s->len)
            {
               for (s = revk_settings; s->len && (!s->group || s->dot != l || strncmp (s->name, tag, l)); s++);
               if (!s->len)
                  return "Not found";
               int group = s->group;
               int index = 0;
               while ((t = jo_next (j)) != JO_CLOSE && index < s->array)
               {
                  for (s = revk_settings; s->len && (!s->group || s->dot != l || strncmp (s->name, tag, l)); s++);
                  if (!s->len)
                     return "Unknown object";
                  if ((err = scan (l, index)))
                     return err;
                  index++;
               }
               while (1)
               {                // Clean up
                  for (s = revk_settings; s->len && (s->group != group || s->array <= index); s++);
                  if (!s->len)
                     break;
                  for (s = revk_settings; s->len; s++)
                     if (s->group == group && s->array >= index)
                        if ((err = store (index)))
                           return err;
                  index++;
               }
               t = JO_NULL;     // Set to default
               for (s = revk_settings; s->len; s++)
                  if (s->group == group && !(found[(s - revk_settings) / 8] & (1 << ((s - revk_settings) & 7))) && !s->secret)
                     zapdef (); // Not secrets, D'Oh
            } else if (!s->array)
               return "Unexpected array";
            else
            {
               int index = 0;
               while (!err && (t = jo_next (j)) != JO_CLOSE && index < s->array)
               {
                  if ((err = store (index)))
                     return err;
                  index++;
               }
               while (!err && index < s->array)
               {                // NULLs
                  if ((err = store (index)))
                     return err;
                  index++;
               }
            }
         } else if ((err = store (pindex)))
            return err;
      }
      return err;
   }
   err = scan (0, -1);
   if (err)
   {
      ESP_LOGE (TAG, "Failed %s at [%s]", err, debug ? : "?");
      jo_t e = jo_make (NULL);
      jo_string (e, "error", err);
      jo_string (e, "location", debug);
      revk_error (prefixsetting, &e);
   }
   if (change)
      revk_restart ("Settings changed", 5);
   return err ? : "";
}

void
revk_settings_commit (void)
{
#ifdef  CONFIG_REVK_SETTINGS_DEBUG
   ESP_LOGE (TAG, "NVC commit");
#endif
   REVK_ERR_CHECK (nvs_commit (nvs[0]));
   REVK_ERR_CHECK (nvs_commit (nvs[1]));
}

#endif