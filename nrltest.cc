#include "nrl.hh"

#include <locale>

#include <print>
#include <unistd.h>


int main()
{
  std::locale loc = std::locale("C.utf8");
  std::locale::global(loc);


  nrl::state s(STDIN_FILENO);

  s.set_prompt("INPUT> ");

  auto l = nrl::read(s);
  std::println("\ninput = {}", l);
}
