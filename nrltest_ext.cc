#include "nrl.hh"

#include <array>
#include <cassert>
#include <cstdlib>
#include <locale>
#include <print>

#include <error.h>
#include <unistd.h>
#include <sys/epoll.h>


int main(int argc, [[maybe_unused]] char* argv[])
{
  std::locale loc = std::locale("C.utf8");
  std::locale::global(loc);

  int epfd = ::epoll_create1(EPOLL_CLOEXEC);
  if (epfd == -1) [[unlikely]]
    ::error(EXIT_FAILURE, errno, "cannot open epoll");

  auto fl = argc == 1 ? nrl::handle::flags::none : static_cast<nrl::handle::flags>(std::atol(argv[1]));

  {
    nrl::handle s(epfd, STDIN_FILENO, fl);

    if (fl == nrl::handle::flags::frame_line)
      s.frame_highlight_fg = {255, 215, 0};

    s.set_prompt("INPUT> ");
    s.empty_message = "Type something …";

    while (true) {
      // We can call prepare unconditionally.
      s.prepare();

      std::array<epoll_event, 1> epev;

      auto n = ::epoll_wait(epfd, epev.data(), epev.size(), -1);
      assert(n != -1);

      auto res = s.process(epev[0]);
      if (res) {
        if (res->empty())
          break;
        std::println("\ninput = {}", *res);
      } else if (! res.error()) {
        std::println("unhandled file descriptor {}", int(epev[0].data.fd));
      }
    }
  }

  close(epfd);
}
