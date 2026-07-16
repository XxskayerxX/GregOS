#include "../include/ports.h"


unsigned char port_byte_in(unsigned short port) {
    unsigned char result;


    __asm__("inb %1, %0" : "=a" (result) : "dN" (port));
    return result;
}


void port_byte_out(unsigned short port, unsigned char data) {
    __asm__("outb %1, %0" : : "dN" (port), "a" (data));
}

void port_word_out(unsigned short port, unsigned short data) {
    __asm__("outw %1, %0" : : "dN" (port), "a" (data));
}

unsigned short port_word_in(unsigned short port) {
    unsigned short result;
    __asm__("inw %1, %0" : "=a" (result) : "dN" (port));
    return result;
}

void port_dword_out(unsigned short port, unsigned int data) {
    __asm__ volatile("outl %0, %1" : : "a" (data), "dN" (port));
}

unsigned int port_dword_in(unsigned short port) {
    unsigned int result;
    __asm__ volatile("inl %1, %0" : "=a" (result) : "dN" (port));
    return result;
}
