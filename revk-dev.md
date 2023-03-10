# ESP32 RevK support library

The `ESP32-RevK` support library is used in almost all of the ESP32 projects on this repository. It provides basic management of WiFi connections, settings, MQTT, and JSON.

This manual covers the use of teh library in an app.

## RevK

This is how an application uses the RevK library.

### Include

To use the libraries simply add the include file
```
#include "revk.h"
```
This includes necessary system includes and `lwmqtt.h` and `jo.h` include files needed.

### CMake

In you `main/CMakeLists.txt ` have
```
set (COMPONENT_REQUIRES "ESP32-RevK)
```

### Init

In `void app_main()` you need, close to the start
```
revk_boot(&app_callback);
```
And once you have done any settings (see below) do
```
revk_start();
```

The `app_callback` is `const char *app_callback(int client, const char *prefix, const char *target, const char *suffix, jo_t j)` which is called for any received MQTT messages. The return value is an empty string for all OK, or an error message. NULL means not handled and not an error (which usually means an error as unknown command).

### Settings

Between `revk_boot` and `revk_start` you should add necessary calls to `revk_register(...)` to add any settings you need.
```
  void revk_register(const char *name,    // Setting name (note max 15 characters inc any number suffix)
                     uint8_t array,       // If non zero then settings are suffixed numerically 1 to array
                     uint16_t size,       // Base setting size, -8/-4/-2/-1 signed, 1/2/4/8 unsigned, 0=null terminated string.
                     void *data,  // The setting itself (for string this points to a char* pointer)
                     const char *defval,  // default value (default value text, or bitmask[space]default)
                     uint8_t flags);      // Setting flags
```
The way this is typically done is a list of settings in a macro, allowing the definition of the settings and the calling of the `revk_register` all to be done from the same list.

For example, define settings like this
```
#define settings                \
        u8(webcontrol,2)        \
        bl(debug)               \
```

You can then define the values, e.g.
```
#define u8(n,d) uint8_t n;
#define bl(n) uint8_t n;
settings
#undef u8
#undef bl
```

And in `app_main` call the `revk_register` like this.
```
#define bl(n) revk_register(#n,0,sizeof(n),&n,NULL,SETTING_BOOLEAN|SETTING_LIVE);
#define u8(n,d) revk_register(#n,0,sizeof(n),&n,str(d),0);
   settings
#undef u8
#undef bl
```

Obviously there could be way more types and flags you can use for different types of settings. This example uses `bl()` for "Boolean, live update", and `u8()` for `uint8_t` with a default value.

### Useful functions tracking state

There are a number of functions to keep track of things...
```
uint32_t revk_link_down(void);  // How long link down (no IP or no parent if mesh)
const char *revk_wifi(void);	// Return wifi SSID
void revk_wifi_close(void); // Close WiFi
int revk_wait_wifi(int seconds); // Wait for WiFi to be ready
char *revk_setting(jo_t); // Store settings
const char *revk_command(const char *tag, jo_t);        // Do an internal command
const char *revk_restart(const char *reason, int delay);        // Restart cleanly
const char *revk_ota(const char *host, const char *target);     // OTA and restart cleanly (target NULL for self as root node)
uint32_t revk_shutting_down(void); // If we are shutting down (how many seconds to go)
```

## LWMQTT

The `lwmqtt` library is a *light weight MQTT* server and client library used internally by the RevK library.
```
lwmqtt_t revk_mqtt(int);	// Return the current LWMQTT handle
void revk_mqtt_close(const char *reason);       // Clean close MQTT
int revk_wait_mqtt(int seconds);	// Wait for MQTT to connect
```

Generally you do not need to directly interact with MQTT, but there are some simple functions to generate MQTT messages.

To use these you construct a JSON object then call these to send the message and free the object.
```
revk_state(const char *suffix,jo_t j); // Send a state message
revk_event(const char *suffix,jo_t j); // Send a event message
revk_info(const char *suffix,jo_t j); // Send a info message
revk_error(const char *suffix,jo_t j); // Send a error message
```
Additional lower level functions are defined in `revk.h` and `lwmqtt.h`

### Example
```
jo_t j = jo_object_alloc();
jo_string(j, "field", tag);
jo_string(j, "error", err);
revk_error("control", &j);
```
## JO

The JSON OBJECT library is designed to allow a JSON object to be constructed or parsed to/from a simple in memory string. This is intended to be very memory efficient as it does not create memory structures for the JSON object itself, and for parsing it literally just scans the string itself with minimum overhead.

The main type used is `jo_t` which is an object to handle either constructing or parsing a JSON string. It is possible to construct a string and then rewind and parse the same string.

The `jo_t` type has a pointer in to the object. For creating this is constantly moving forward adding fields and values. For parsing it is possible to rewind and go back and move forward through the object.

Use `jo_free` to free an object, but some functions such as `revk_info`, etc, free for you.

### Error handling

If at any point there is an error, e.g. bad parsing, or bad creating, or memory overrun creating then an error flag is set. You can check with `jo_error` to see if an error. Once set parsing stops (`JO_END` returned) and creating stops (no action).

### Parsing JSON

To parse a JSON object in memory you start with `jo_parse_mem` which is given a buffer and length.

To move through the object you can use `jo_here` to tell what is at this point, `jo_next` to move to next point and tell what is at that point, `jo_skip` to skip the next value at the same level, e.g. if the next value is an object it skips the whole object, and finally `jo_find` to find a named field in the JSON where you pass a tag that can be *tag*, or *tag.tag*, etc, returning the type of the value it finds.

The type of where you are can be one of:-

|Type|Meaning|
|----|-------|
|`JO_END`|End of JSON parsing|
|`JO_CLOSE`|End of object or array, i.e. `}` or `]`|
|`JO_TAG`|The name/tag in an object|
|`JO_OBJECT`|The `{` at the start of an object|
|`JO_ARRAY`|The `[` at the start of an array|
|`JO_STRING`|A string value|
|`JO_NUMBER`|A number value|
|`JO_NULL`|A `null` value|
|`JO_TRUE`|A `true` value|
|`JO_FALSE`|A `faluse` value|

Walking through an object using `jo_next` sees the start (`JO_OBJECT` or `JO_ARRAY`) and end (`JO_CLOSE`) of objects and arrays, and in objects it sees the `JO_TAG` and then the value type of that tag.

Once on the value, e.g. after a `JO_TAG` or within an array, you can get the value using functions. For literals you see if `JO_TRUE`/`JO_FALSE`/`JO_NULL` and can get values using `jo_read_int` or `jo_read_float`.

However strings are more complex as the raw JSON has escaping. `jo_strlen` gives the length of a `JO_STRING` value after de-escaping. `jo_strncpy` can be used to copy and de-escape. `jo_strncmp` can be used to compare to a normal string. `jo_strdup` can be used to copy and de-escape in to malloc'd memory. These string functions can be used at a `JO_STRING` or `JO_TAG` point.

### Creating JSON

You create a new object for creating a JSON object using either `jo_create_alloc` which returns an empty JSON object ready to create, or `jo_object_alloc` which returns a JSON structure which has opened an object, i.e. the initial `{` exists and a final `}` will be added when closed. These allocate memory using `malloc` and `realloc` as needed. You can also create one using static memory using `jo_create_mem` which is passed a buffer and length.





