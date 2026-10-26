#pragma once
/* Stage 9: bit-banged I2C0 scan for the touch IC. mask bits: 1 A, 2 B, 4 C. */
void i2c_stage9(int mask);
