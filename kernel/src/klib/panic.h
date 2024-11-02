#include "../print/e9print.h"


inline void Panic(const char* message, uint8_t halt){
    e9_print("Panic!");
    e9_printf(message);
    hcf();
}