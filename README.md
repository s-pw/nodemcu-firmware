# **PoC CA80 Emulator of ESP8266** #

# Summary

A proof of concept emulator of Z80 based CA80 microcomputer.

History of microcomputer can be found on [http://www.tomsautomation.eu/historyca80.html](http://www.tomsautomation.eu/historyca80.html)

# Hardware

Connect GPIO13 to SCL, GPIO12 to SDA ot the display.

Display supported by defauilt: sh1106_i2c_128x64_noname.

There is no hardware keyboard, keystrokes should be send over the network and fed `ca80.key([keyChar])`

# Documentation 

[NodeMCU documentation](https://nodemcu.readthedocs.io)
- How to [build the firmware](https://nodemcu.readthedocs.io/en/master/en/build/)
- How to [flash the firmware](https://nodemcu.readthedocs.io/en/master/en/flash/)
- How to [upload code and NodeMCU IDEs](https://nodemcu.readthedocs.io/en/master/en/upload/)
- API documentation for every module
