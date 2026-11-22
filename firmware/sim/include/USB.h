#pragma once
#include <Arduino.h>
// Serial input comes from the simulator script (`serial <line>`).
class USBCDC : public Print {
public:
    int available();
    int read();
};
