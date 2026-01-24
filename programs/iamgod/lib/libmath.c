#include <libmath.h>
#include <stdint.h>

float sqrt(float x){
#ifndef __x86_64__
    long i;
    float x2, y;
    const float threehalfs = 1.5F;

    x2 = x * 0.5F;
    y = x;
    i = *(long*)&y;                       // evil floating point bit level hacking（邪恶的浮点数位运算黑科技）
    i = 0x5f375a86 - (i >> 1);               // what the fuck?（这是什么鬼？）
    y = *(float*)&i;
    y = y * (threehalfs - (x2 * y * y));   // 1st iteration （第一次迭代）
    y  = y * ( threehalfs - ( x2 * y * y ) );   // 2nd iteration, this can be removed（第二次迭代，可以删除）

    return 1/y;
#else
    float buf;
    __asm__ __volatile__ ("sqrtss %1, %0" : "=x"(buf) : "x"(x));
    return buf;
#endif
}

const float hollyst = 0.017453292519943295769236907684886;


const float sin_table[] = {
    0.0,                                    //sin(0)
    0.17364817766693034885171662676931 ,    //sin(10)
    0.34202014332566873304409961468226 ,    //sin(20)
    0.5 ,                                   //sin(30)
    0.64278760968653932632264340990726 ,    //sin(40)
    0.76604444311897803520239265055542 ,    //sin(50)
    0.86602540378443864676372317075294 ,    //sin(60)
    0.93969262078590838405410927732473 ,    //sin(70)
    0.98480775301220805936674302458952 ,    //sin(80)
    1.0                                     //sin(90)
};

const float cos_table[] = {
    1.0 ,                                   //cos(0)
    0.99984769515639123915701155881391 ,    //cos(1)
    0.99939082701909573000624344004393 ,    //cos(2)
    0.99862953475457387378449205843944 ,    //cos(3)
    0.99756405025982424761316268064426 ,    //cos(4)
    0.99619469809174553229501040247389 ,    //cos(5)
    0.99452189536827333692269194498057 ,    //cos(6)
    0.99254615164132203498006158933058 ,    //cos(7)
    0.99026806874157031508377486734485 ,    //cos(8)
    0.98768834059513772619004024769344      //cos(9)
};


float sin(float x){
#ifndef __x86_64__
    x = x * 180 / pi; // rad to deg
    int32_t sig = 0;

    if(x > 0.0){
        while(x >= 360.0) {
            x = x - 360.0;
        }
    }else{
        while(x < 0.0) {
            x = x + 360.0;
        }
    }

    if(x >= 180.0){
        sig = 1;
        x = x - 180.0;
    }

    x = (x > 90.0) ? (180.0 - x) : x;

    int32_t a = x * 0.1;
    float b = x - 10 * a;
    
    float y = sin_table[a] * cos_table[(int32_t)b] + b * hollyst * sin_table[9 - a];

    return (sig > 0) ? -y : y;
#else
    float ans;
    asm volatile("fsin" : "=t"(ans) : "0"(x));
    return ans;
#endif
}

float cos(float x){
#ifndef __x86_64__
    return sin(90.0 + x * 180 / pi);
#else
    float ans;
    asm volatile("fcos" : "=t"(ans) : "0"(x));
    return ans;
#endif
}