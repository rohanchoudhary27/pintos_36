/* Executes and waits for multiple child processes. */

#include "tests/lib.h"
#include "tests/main.h"
#include <stdio.h>
#include <syscall.h>

void
test_main (void) 
{
  printf("wait exec the first simple\n");
  wait (exec ("child-simple"));
  printf("wait exec the second simple\n");
  wait (exec ("child-simple"));
  printf("wait exec the third simple\n");
  wait (exec ("child-simple"));
  printf("wait exec the fourth simple\n");
  wait (exec ("child-simple"));
}
