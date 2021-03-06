#include<dev/console.h>
#include<lib/util.h>
#include<lib/stdio.h>
#include<sync/spinlock.h>
#include<proc/cpu.h>
static struct spinlock printflock;
static void printnum(int64_t num, uint64_t base){
    if(num < 0 && base == 10){
        consoleput('-');
        num = -num;
    }
    char digits[17] = "0123456789abcdef";
    uint64_t buf[20] = {0};
    for(int64_t i = 0; i < 20; i++){
        buf[i] = num % base;
        num = num / base;
    }
    int64_t ptr = 19;
    if(base == 16){
        ptr = 15;
    }else{
        while(buf[ptr] == 0 && ptr > 0){
            ptr--;
        }
    }
    for(int64_t i = ptr; i >= 0; i--){
        consoleput(digits[buf[i]]);
    }
}
void printf(char* fmt, ...){
    if(systemstarted){
        acquire(&printflock);
    }
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for(int64_t i = 0; fmt[i] != 0; i++){
        if(fmt[i] != '%' || fmt[i + 1] == 0){
            consoleput(fmt[i]);
        }else{
            i++;
            switch(fmt[i]){
                case '%':
                    consoleput('%');
                    break;
                case 'd':
                    printnum(__builtin_va_arg(ap, unsigned long), 10);
                    break;
                case 'x':
                    printnum(__builtin_va_arg(ap, unsigned long), 16);
                    break;
                case 's':
                    for(char* s = __builtin_va_arg(ap, char*); *s; s++){
                        consoleput(*s);
                    }
                    break;
                default:
                    consoleput('%');
                    consoleput(fmt[i]);
                    break;
            }
        }
    }
    if(systemstarted){
        release(&printflock);
    }
}