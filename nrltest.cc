#include "nrl.hh"

#include <locale>

#include <print>
#include <unistd.h>


int main(int argc, [[maybe_unused]] char* argv[])
{
  std::locale loc = std::locale("C.utf8");
  std::locale::global(loc);

  auto fl = argc == 1 ? nrl::handle::flags::none : static_cast<nrl::handle::flags>(std::atol(argv[1]));

  nrl::handle s(STDIN_FILENO, fl);

  if (fl == nrl::handle::flags::frame_line)
    s.frame_highlight_fg = {255, 215, 0};

  s.set_prompt("INPUT> ");
  s.empty_message = "Type something …";

  while (true) {
    auto l = s.read();
    if (l.empty())
      break;
    std::println("input = {}", l);
  }
}
