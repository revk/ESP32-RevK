// LED strip controller
// This uses SPI to generate WS2812 style pulses to work with a variety of LEDs.
// The design allows strips in any order of R, G, B, W so working with a variety of devices
// The design also allows chained strips of different types, e.g. an RGB chained to an RGBW
// However it cannot mix different clock (WS2812/SK6812) or inverts on same GPIO

#ifdef	CONFIG_REVK_LED

typedef struct led_strip_s *led_strip_t;
enum
{                               // LED order (for first 3 colours)
   LED_GRB,
   LED_GBR,
   LED_RGB,
   LED_RBG,
   LED_BGR,
   LED_BRG,
};

#ifdef	REVK_LED_FULL
enum
{
   LED_WS2812,                  // 400/800ns and 800/400ns
   LED_SK6812,                  // 300/900ns and 600/600ns
   LED_XINGLIGHT,               // 300/900ns and 900/300ns
   LED_TYPES
};
#endif

// Functions return NULL if OK, else error message

// This frees any existing resources, invalidating any previous strip handles.
// Do not call during led_send().
const char *led_reset (int spi_host);

// This adds a new strip. If multiple strips on the same GPIO, add in order. Sets *strip
// Do not call during led_send().
const char *led_strip(led_strip_t * strip,       // Where to store strip handle (stores NULL if error)
                     uint8_t gpio,      // GPIO
                     uint8_t invert,    // GPIO invert
#ifdef	REVK_LED_FULL
                     uint8_t type,      // Strip type (for timing)
#endif
                     uint16_t leds,     // Number of LEDs
                     uint8_t colours,   // Number of colours, normally 3 or 4, but can be more
                     uint8_t order      // RGB Colour order
   );

// Clear strip (all LEDs off)
const char *led_clear(led_strip_t strip);

// Set value of LED, called with three uint8_t for R/G/B, or more if more than 3 colours on strip, e.g. R/G/B/W
const char *led_set (led_strip_t strip, uint16_t led, ...);

// This updates all strips
const char *led_send (void);

#endif
