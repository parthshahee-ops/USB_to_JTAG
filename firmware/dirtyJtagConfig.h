#ifndef DirtyJtagConfig_h
#define DirtyJtagConfig_h

#define BOARD_PICO           0
#define BOARD_ADAFRUIT_ITSY  1
#define BOARD_SPOKE_RP2040   2
#define BOARD_QMTECH_RP2040_DAUGHTERBOARD 3
#define BOARD_WERKZEUG       4
#define BOARD_RP2040_ZERO    5

#ifndef BOARD_TYPE
#define BOARD_TYPE BOARD_PICO
#endif

#if ( BOARD_TYPE == BOARD_PICO )

// === HARDWARE INTERFACING SPI PINOUT (SHRIKE LITE INTERNAL) ===
#define SPI_PORT     spi0
#define PIN_SPI_MISO 0
#define PIN_SPI_CS   1
#define PIN_SPI_SCK  2
#define PIN_SPI_MOSI 3

// Power, Enable, and Target Reset Signals
// BUG FIX: PIN_TARGET_RST is now a named macro so it is explicitly managed
//          at the very start of main() — before power, before SPI — preventing
//          the reset line from floating during the bitstream deploy phase.
#define PIN_PWR        12
#define PIN_EN         13
#define PIN_TARGET_RST 14  // Active-Low reset for target hardware (asserted LOW first)

// Dummy JTAG pins (keep so the compiler does not throw "undefined" errors;
// these are wired to unused GPIOs and are never electrically driven)
#define PIN_TCK  26
#define PIN_TDI  27
#define PIN_TMS  28
#define PIN_TDO  29
#define PIN_RST  22
#define PIN_TRST 23

// On-Board Status LED (all three roles share one physical LED on the Pico)
#define LED_INVERTED  0
#define PIN_LED_TX    4
#define PIN_LED_ERROR 4
#define PIN_LED_RX    4

// UART bridge disabled — pure SPI bridge mode
#define CDC_UART_INTF_COUNT 0

#elif ( BOARD_TYPE == BOARD_ADAFRUIT_ITSY )

#if defined(BOARD_ADAFRUIT_ITSY_KB2040)
#define PIN_TDI  4
#define PIN_TDO  3
#define PIN_TCK  2
#define PIN_TMS  5
#define PIN_RST  6
#define PIN_TRST 7
#else
#define PIN_TDI  28
#define PIN_TDO  27
#define PIN_TCK  26
#define PIN_TMS  29
#define PIN_RST  24
#define PIN_TRST 25
#endif

#define LED_INVERTED  0
#define PIN_LED_TX    -1
#define PIN_LED_ERROR -1
#define PIN_LED_RX    -1

#define CDC_UART_INTF_COUNT 1

#if defined(BOARD_ADAFRUIT_ITSY_KB2040)
#define PIN_UART0    uart1
#define PIN_UART0_TX 8
#define PIN_UART0_RX 9
#else
#define PIN_UART0    uart0
#define PIN_UART0_TX 0
#define PIN_UART0_RX 1
#endif

#elif ( BOARD_TYPE == BOARD_SPOKE_RP2040 )

#define PIN_TDI  3
#define PIN_TDO  5
#define PIN_TCK  2
#define PIN_TMS  4
#define PIN_RST  26
#define PIN_TRST 27

#define LED_INVERTED  1
#define PIN_LED_TX    16
#define PIN_LED_ERROR 17
#define PIN_LED_RX    18

#define CDC_UART_INTF_COUNT 1
#define PIN_UART0    uart0
#define PIN_UART0_TX 28
#define PIN_UART0_RX 29

#elif ( BOARD_TYPE == BOARD_WERKZEUG )

#define PIN_TDI  1
#define PIN_TDO  2
#define PIN_TCK  0
#define PIN_TMS  3
#define PIN_RST  4
#define PIN_TRST 5

#define LED_INVERTED  1
#define PIN_LED_TX    20
#define PIN_LED_ERROR 21
#define PIN_LED_RX    20

#define CDC_UART_INTF_COUNT 1
#define PIN_UART0    uart0
#define PIN_UART0_TX 28
#define PIN_UART0_RX 29

#elif ( BOARD_TYPE == BOARD_QMTECH_RP2040_DAUGHTERBOARD )

#define CDC_UART_INTF_COUNT 0

#define PIN_TDI 16
#define PIN_TDO 17
#define PIN_TCK 18
#define PIN_TMS 19

#define LED_INVERTED  0
#define PIN_LED_TX    25
#define PIN_LED_ERROR 25
#define PIN_LED_RX    25

#elif ( BOARD_TYPE == BOARD_RP2040_ZERO )

#define PIN_TDI  0
#define PIN_TDO  3
#define PIN_TCK  2
#define PIN_TMS  1
#define PIN_RST  4
#define PIN_TRST 5

#define LED_INVERTED  1
#define PIN_LED_TX    29
#define PIN_LED_ERROR 29
#define PIN_LED_RX    29

#define CDC_UART_INTF_COUNT 2
#define PIN_UART0    uart0
#define PIN_UART0_TX 12
#define PIN_UART0_RX 13
#define PIN_UART1    uart1
#define PIN_UART1_TX 8
#define PIN_UART1_RX 9

#endif // BOARD_TYPE

#endif // DirtyJtagConfig_h