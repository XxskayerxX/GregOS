#ifndef PORTS_H
#define PORTS_H

#ifdef __cplusplus
extern "C" {
#endif

unsigned char  port_byte_in (unsigned short port);
void           port_byte_out(unsigned short port, unsigned char data);
void           port_word_out(unsigned short port, unsigned short data);
unsigned short port_word_in (unsigned short port);
void           port_dword_out(unsigned short port, unsigned int data);
unsigned int   port_dword_in (unsigned short port);

#ifdef __cplusplus
}
#endif

#endif
