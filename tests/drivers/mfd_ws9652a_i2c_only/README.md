# Generic I2C PMU fixture

This fixture instantiates the production WS9652A MFD on a scripted generic I2C
controller that initializes at `POST_KERNEL`, `CONFIG_I2C_INIT_PRIORITY`.
There is no direct bridge or WS9652A I2C controller in devicetree or the build.

It checks PMU and ADC child initialization order and readiness without bus
transactions, ADC channel configuration without I/O, explicit preparation through
standard I2C, identity reads and child transaction callbacks, output preservation
on read failure, and rejection of a runtime C7 mode mismatch. The fake byte map
exercises transport integration; it does not model PMU analog behavior or establish
hardware support for reverse handover.
