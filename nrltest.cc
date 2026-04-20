#include "nrl.hh"

#include <array>
#include <cassert>
#include <cerrno>
#include <csignal> // IWYU pragma: keep
#include <cstdlib>
#include <locale>
#include <print>

#include <error.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>


namespace {

  std::unique_ptr<nrl::handle> new_input(int epfd, nrl::handle::flags fl)
  {
    auto res = std::make_unique<nrl::handle>(epfd, STDIN_FILENO, fl);
    if (fl == nrl::handle::flags::frame_line)
      res->frame_highlight_fg = {255, 215, 0};

    res->set_prompt("\e[31mINPUT\e[0m> ");
    res->empty_message = "Type something …";

    return res;
  }

} // anonymous namespace


int main(int argc, char* argv[])
{
  std::locale loc = std::locale("C.utf8");
  std::locale::global(loc);

  int epfd = ::epoll_create1(EPOLL_CLOEXEC);
  if (epfd == -1) [[unlikely]]
    ::error(EXIT_FAILURE, errno, "cannot open epoll");

  sigset_t mask;
  ::sigemptyset(&mask);
  ::sigaddset(&mask, SIGHUP);
  ::sigprocmask(SIG_BLOCK, &mask, nullptr);

  int sfd = ::signalfd(-1, &mask, SFD_NONBLOCK);
  if (sfd == -1) [[unlikely]]
    ::error(EXIT_FAILURE, errno, "cannot create signal fd");

  ::epoll_event event{.events = EPOLLIN, .data = {.fd = sfd}};
  if (::epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &event) != 0) [[unlikely]]
    ::error(EXIT_FAILURE, errno, "cannot register signal fd");

  auto fl = argc == 1 ? nrl::handle::flags::none : static_cast<nrl::handle::flags>(std::clamp(std::atol(argv[1]), 0l, 2l));

  {
    auto ps = new_input(epfd, fl);

    ps->prepare({"otherwise", "option #1", "option #2"}, true);

    while (true) {
      std::array<epoll_event, 1> epev;

      auto n = ::epoll_wait(epfd, epev.data(), epev.size(), -1);
      if (n != 1)
        continue;

      if (epev[0].data.fd == sfd) {
        ::signalfd_siginfo ssi;
        while (::read(sfd, &ssi, sizeof(ssi)) > 0) {
          // Ignore the data.
        }
        ::write(STDOUT_FILENO, "\e[m\e[0J", 7);
        ::sleep(2);
        ps->redraw();
      } else {
        auto res = ps->process(epev[0]);
        if (res) {
          auto pos = terminal::info::get_cursor_pos(STDOUT_FILENO);
          ::write(STDOUT_FILENO, "\e[10A\e[10L", 10);
          ps->redraw();
          if (pos) {
            auto spos = std::format("\e[{};{}H", std::get<1>(*pos), std::get<0>(*pos));
            ::write(STDOUT_FILENO, spos.data(), spos.size());
          }

          if (res->empty())
            break;
          std::println("input = {}", *res);

          ps = new_input(epfd, fl);
          ps->prepare();
        } else if (! res.error())
          std::println("unhandled file descriptor {}", int(epev[0].data.fd));
      }
    }
  }

  close(epfd);
}
