#include "nrl.hh"

#include <locale>

#include <print>
#include <unistd.h>


int main(int argc, [[maybe_unused]] char* argv[])
{
  std::locale loc = std::locale("C.utf8");
  std::locale::global(loc);

  auto fl = argc == 1 ? nrl::state::flags::none : static_cast<nrl::state::flags>(std::atol(argv[1]));

  nrl::state s(STDIN_FILENO, fl);

  if (fl == nrl::state::flags::frame_line)
    s.frame_highlight_fg = {255, 215, 0};

  s.set_prompt("INPUT> ");

  auto l = nrl::read(s);
  std::println("\ninput = {}", l);
}
