# DrinkBot Controller Spec

## Overview
The DrinkBotController will be a XIAO ESP32-S3 plugged into a DrinkBotPump via USB. The DrinkBotPump is a USB serial device so the ESP32-S3 USB port shall be configured in host mode and the interface treated as a virtual UART. The DrinkBotController will use one DrinkBotPump as a bridge to talk to all the pumps in the system. 

The DrinkBotController hardware shall have 7 physical buttons attached to GPIO pins. These pins should be active low with internal pull-ups enabled.
 
The DrinkBotController hardware shall have 9 addressable NeoPixels in a chain attached to a GPIO pin. 

DrinkBotController application will handle dispensing custom mixed drinks on command by instructing each pump to one at a time dispense a particular amount of liquid in a particular order into a glass. 

### Pin Mapping

| XIAO Pin | ESP32-S3 GPIO | Mapped Function |
| D0       | GPIO 1        | Button 1        |
| D1       | GPIO 2        | Button 2        |
| D3       | GPIO 4        | Button 3        |
| D4       | GPIO 5        | Button 4        |
| D5       | GPIO 6        | Button 5        |
| D8       | GPIO 7        | Button 6        |
| D9       | GPIO 9        | Button 7        |
| D10      | GPIO 8        | LED Data Output |


## Configuration Data
The DrinkBotController shall have a config file that is stored as an onboard json file. This file will persist across resets and power cycles.

The config shall contain:
- A list of recipes for drinks. Each recipe will have:
    - A drink name 
    - A list of ingredients. Each listed ingredient will have:
        -  A name 
        -  A unitless number that is proportional to its amount in the recipe
        -  An index number controlling the order in which it should be added 
- A list of pump/ingredients available. Each ingredient will have:
    - A string name
    - A numerical value that is the pump index matching the id of the pump it is loaded into
    - A numerical value that is the milliseconds per milliliter for that liquid
    - A numerical value that is the grams to milliliter for that liquid
    - An RGB color that shall be used when dispensing that liquid
- A button assignment that connects a physical button to an action. Actions are: 
    - Set drink size to X milliliters
    - Set active recipe to X
    - Dispense active recipe
    - E-Stop all pump activity
- A list of wireless networks with their credentials to which the host should attempt to connect
- A liquid level in milliliters at which a pump is indicated low
- A liquid level in milliliters at which a pump is indicated empty
- A liquid level in milliliters at which a pump is actually empty

## Web Interface
The DrinkBotController on the host will have a built in web server and will serve a simple interactive web app. It shall not be assumed that an internet connection will be available so all necessary support files must also be served by the internal web server. The web server shall connect to the served web app using web sockets to allow for streaming data in both directions. JSON formatted messages shall be used over the web socket. No security is needed for this web interface at this time.

- The web interface shall have a realtime log view where all application log outputs are streamed in real time. Log view should auto scroll to keep the latest log lines in view. There should be a button to clear the log view.
- The web interface shall allow for the stored recipe list to be graphically edited and saved.
- The web interface shall allow for the pump list to be graphically edited and saved.
- The web interface shall allow for button assignments to be graphically edited and saved.
- The web interface shall allow for the wireless AP list to be graphically edited and saved.
- The web interface shall provide a button to allow a specified recipe to be dispensed. 
- The web interface shall provide a settable field that is the total amount of liquid to be dispensed in milliliters. When dispensing the requested recipe this total volume of liquid shall be used along with the individual ingredient amounts as ratios to determine what portion of the output it should be and how many milliliters should be dispensed.
- The web interface should have a control that allows any pump channel to be primed for a specified number of seconds. The default number of seconds shall be 5.
- The web interface should have a control that allows any pump channel to be purged for a specified number of seconds. The default number of seconds shall be 10. 
- The web interface shall have a means of rebooting the host with a button press.
- The web interface shall have a representation of the current state of the 10 UI LEDs that reflects their current state on the hardware device.
- The web interface shall have visual representation of each of the attached pumps. It should indicate their current status, weight, firmware version, ID, loaded liquid name, and provide a visual approximation of its remote LED state.

The web interface shall be designed so that log view is always visible in the lower part of the browser window. The view shall be vertically resizable using mouse drags.

## System Behaviors
The DrinkBotController shall include robust logging that captures all actions and errors of the application. This log shall be streamed to a UART interface as well as to the web interface.

On power up the DrinkBotController shall attempt to join one of the wireless networks listed in the config file. If no wireless connection can be made after 20 seconds the host should form its own open wireless network with the SSID of DrinkBot. 

On power up the DrinkBotController shall query each of the pumps identified in the pump list and request their version to confirm their existence on the bus. If one of the pumps is not found the system shall enter an error state.

Before dispensing any liquid the host shall request the weight from that pump and confirm there is sufficient liquid in the pump to meet the requested dispensing amount and not reach the actually empty level. When a pump runs low on liquid its LED lights should be set in a slow yellow pulse state. This state shall be restored after any dispense operation. When a pump is empty its LED lights should be set in a slow yellow red state.

For initial testing and development the DrinkBotController will have a simulator mode where no actual commands are sent to the pumps. Instead 8 virtual pumps will be instantiated and used to test the DrinkBotController software before being attached to the pump network. The system shall enter simulator mode anytime no connected USB CDC device is detected on power up. 

## Visual UX
All state communication is performed using the NeoPixel LEDs. One LED will be located next to each button. When that button is pressed the LED should be illuminated green. For the buttons that select the recipe the LED next to the active recipe shall be red, the rest shall be off. For the buttons that select the drink size options the LED next to the active size shall be red, the rest shall be off. The LEDs that are not used for buttons shall display system status. One is the Status LED: green when booted and ready, blinks yellow while dispensing, blinks red on error. One is the Wifi LED: blinking green while searching, solid green when connected, solid yellow if in AP mode. 

| LED | Function |
| 1   | Button 1 |
| 2   | Button 2 |
| 3   | Button 3 |
| 4   | Button 4 |
| 5   | Button 5 |
| 6   | Button 6 |
| 7   | Button 7 |
| 8   | Status   |
| 9   | WIFI     |

## Resources
The Arduino code for the DrinkBotPump is in the project directory.
