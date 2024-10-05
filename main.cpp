#include "stdio.h"
#include "sys/mman.h"
#include "errno.h"
#include "unistd.h"
#include "string.h"
#include "elfio/elfio.hpp"

void banana()
{
  printf("banana\n");
}

void apple()
{
  printf("apple\n");
}

int main()
{
  void* aligned = (void*)((size_t)&apple & -getpagesize());

  if (mprotect(aligned, 8, PROT_WRITE | PROT_EXEC | PROT_READ))
    printf("err in mprotect: %i\n", errno);

  unsigned char code[16];

  int offset = 0;

  auto writeREX = [&](bool w, bool r, bool x, bool b)
  {
    code[offset] = 0x40 | (w << 3) | (r << 2) | (r << x) | (r << b);
    offset += 1;
  };

  auto writeByte = [&](char b)
  {
    code[offset] = b;
    offset += 1;
  };

  auto writeImm64 = [&](long x)
  {
    writeByte((x >>  0) & 0xff);
    writeByte((x >>  8) & 0xff);
    writeByte((x >> 16) & 0xff);
    writeByte((x >> 24) & 0xff);
    writeByte((x >> 32) & 0xff);
    writeByte((x >> 40) & 0xff);
    writeByte((x >> 48) & 0xff);
    writeByte((x >> 56) & 0xff);
  };

  // mov r11, &banana
  writeREX(true, false, false, true);
  writeByte(0xbb);
  writeImm64((long)&banana);

  // call r11
  writeREX(false, false, false, true);
  writeByte(0xff);
  writeByte(0xd3);

  // ret
  writeByte(0xc3);

  memcpy((void*)&apple, code, sizeof(code));

  apple();
}
