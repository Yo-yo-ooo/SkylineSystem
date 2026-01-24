#include <stdint.h>
#include <stddef.h>
//in project simplified memcpy, can handle overlap
void *IPSMemcpy(void *dest, const void *src, size_t count)  
{  
    int32_t bytelen=count/sizeof(dest); /*按CPU位宽拷贝*/
    int32_t slice=count%sizeof(dest); /*剩余的按字节拷贝*/
    uint32_t* d = (uint32_t*)dest;  
    uint32_t* s = (uint32_t*)src;  

    if (((int32_t)dest > ((int32_t)src+count)) || (dest < src))  {  
        while (bytelen--)  
            *d++ = *s++;  
        while (slice--)  
            *(int8_t *)d++ = *(int8_t *)s++; 
    }  else /* overlap重叠 */  {  
        d = (uint32_t*)((uint32_t)dest + count - 4); /*指针位置从末端开始，注意偏置 */  
        s = (uint32_t*)((uint32_t)src + count -4);  
        while (bytelen --)  
            *d-- = *s--;  
        d++;s++;
        int8_t * d1=(int8_t *)d;
        int8_t * s1=(int8_t *)s;
        d1--;s1--;
        while (slice --)  
            *(int8_t *)d1-- = *(int8_t *)s1--;   
    }
    return dest;  
}  