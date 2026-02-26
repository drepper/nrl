#include "nrl.hh"
#include <cstddef>
#include <filesystem>
#include <system_error>

#if defined __cpp_modules && __cpp_modules >= 201810L
import std;
#else
# include <algorithm>
# include <array>
# include <cassert>
# include <cerrno>
# include <compare>
# include <csignal> // IWYU pragma: keep
# include <cstdint>
# include <cstdlib>
# include <cstring>
# include <format>
# include <map>
#endif

#include <error.h>
#include <fcntl.h>
#include <string_view>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/uio.h>

#include <unictype.h>
#include <unistr.h>

#include "termdetect/termdetect.hh"

// Debug
// #include <print>


namespace nrl {

  namespace {

    // Return the length of the visible characters of the string.  ANSI escape sequences are not counted.
    // At this time only CSI sequences have to be handled.  The encoding is known to be UTF-8.
    size_t nonescape_len(const std::string_view sv)
    {
      bool in_csi = false;
      size_t res = 0;
      for (char ch : sv)
        if (! in_csi) {
          if (ch == '\x1b') [[unlikely]]
            in_csi = true;
          else if ((ch & 0xc0) != 0x80)
            ++res;
        } else
          // CSI sequences end with a character in the range 0x40 ('@') to 0x7e ('~').
          in_csi &= ch == '[' || static_cast<uint8_t>(ch) < 0x40 || static_cast<uint8_t>(ch) > 0x7e;

      return res;
    }


    std::tuple<unsigned, unsigned> update_winsize(int fd)
    {
      winsize ws;
      if (::ioctl(fd, TIOCGWINSZ, &ws) != -1)
        return {ws.ws_col, ws.ws_row};

      // This should only happen in case the file descriptor is not for a terminal at which
      // point the visual functionality provided by this library cannot work anyway.  Return
      // something "normal".
      return {80u, 25u};
    }


    struct hsv_color {
      uint8_t h;
      uint8_t s;
      uint8_t v;
    };


    // The conversion functions are taken from a StackOverflow posting by Leszek Szary
    //    https://stackoverflow.com/a/14733008
    // This code has been converted here to modern C++.
    terminal::info::color hsv_to_rgb(hsv_color hsv)
    {
      terminal::info::color rgb{.r = hsv.v, .g = hsv.v, .b = hsv.v};

      if (hsv.s != 0) {
        uint8_t region = hsv.h / 43;
        uint8_t remainder = (hsv.h - (region * 43)) * 6;

        uint8_t p = (hsv.v * (255 - hsv.s)) >> 8;
        uint8_t q = (hsv.v * (255 - ((hsv.s * remainder) >> 8))) >> 8;
        uint8_t t = (hsv.v * (255 - ((hsv.s * (255 - remainder)) >> 8))) >> 8;

        switch (region) {
        case 0:
          rgb = {.r = hsv.v, .g = t, .b = p};
          break;
        case 1:
          rgb = {.r = q, .g = hsv.v, .b = p};
          break;
        case 2:
          rgb = {.r = p, .g = hsv.v, .b = t};
          break;
        case 3:
          rgb = {.r = p, .g = q, .b = hsv.v};
          break;
        case 4:
          rgb = {.r = t, .g = p, .b = hsv.v};
          break;
        default:
          rgb = {.r = hsv.v, .g = p, .b = q};
          break;
        }
      }

      return rgb;
    }


    hsv_color rgb_to_hsv(terminal::info::color rgb)
    {
      uint8_t rgb_min = std::min({rgb.r, rgb.g, rgb.b});
      uint8_t rgb_max = std::max({rgb.r, rgb.g, rgb.b});

      hsv_color hsv{.h = 0, .s = 0, .v = rgb_max};
      if (hsv.v != 0) {
        hsv.s = 255 * (rgb_max - rgb_min) / hsv.v;
        if (hsv.s == 0)
          hsv.h = 0;
        else if (rgb_max == rgb.r)
          hsv.h = 0 + 43 * (rgb.g - rgb.b) / (rgb_max - rgb_min);
        else if (rgb_max == rgb.g)
          hsv.h = 85 + 43 * (rgb.b - rgb.r) / (rgb_max - rgb_min);
        else
          hsv.h = 171 + 43 * (rgb.r - rgb.g) / (rgb_max - rgb_min);
      }

      return hsv;
    }


    std::tuple<terminal::info::color, terminal::info::color> adjust_rgb(terminal::info::color fg, terminal::info::color bg, int adjust_val)
    {
      auto hsv_fg = rgb_to_hsv(fg);
      auto hsv_bg = rgb_to_hsv(bg);
      if (adjust_val >= 0 ? (hsv_bg.v >= 128) : (hsv_bg.v < 128)) {
        hsv_fg.v = hsv_fg.v > adjust_val ? (hsv_fg.v - adjust_val) : 0;
        hsv_bg.v -= adjust_val;
      } else {
        hsv_fg.v = hsv_fg.v < (255 - adjust_val) ? (hsv_fg.v + adjust_val) : 255;
        hsv_bg.v += adjust_val;
      }
      return std::make_tuple(hsv_to_rgb(hsv_fg), hsv_to_rgb(hsv_bg));
    }


    std::tuple<unsigned, unsigned> get_current_pos(int fd)
    {
      static const char dsr[] = "\e[6n";
      ::write(fd, dsr, strlen(dsr));

      unsigned col = 0;
      unsigned row = 0;

      int oldfl = ::fcntl(fd, F_GETFL);
      ::fcntl(fd, F_SETFL, oldfl & ~O_NONBLOCK);

      char buf[256];
      bool done = false;
      while (! done) {
        auto n = ::read(fd, buf, sizeof(buf));
        if (n == -1)
          break;
        else {
          ssize_t i = 0;
          while (i + 6 <= n) {
            if (buf[i] == '\e' && buf[i + 1] == '[' && isdigit(buf[i + 2])) {
              row = 0;
              auto cp = &buf[i + 2];
              while (cp < buf + n && isdigit(*cp))
                row = row * 10 + (*cp++ - '0');
              if (*cp++ == ';' && isdigit(*cp)) {
                col = 0;
                while (cp < buf + n && isdigit(*cp))
                  col = col * 10 + (*cp++ - '0');
                if (*cp == 'R') {
                  done = true;
                  break;
                }
              }
              row = col = 0;
            }
            ++i;
          }
        }
      }

      ::fcntl(fd, F_SETFL, oldfl);

      return {col, row};
    }


    const char osc133_L[] = "\e]133;L\a";
    const char osc133_A[] = "\e]133;A\a";
    const char osc133_B[] = "\e]133;B\a";
    const char osc133_C[] = "\e]133;C\a";


    auto move_to_buf(char* buf, size_t n, handle& s, int x, int y)
    {
      return snprintf(buf, n, "\e[%u;%uH", s.initial_row + y, s.initial_col + x);
    }


    void move_to(handle& s, int x, int y)
    {
      char buf[40];
      auto n = move_to_buf(buf, sizeof(buf), s, x, y);
      ::write(s.fd, buf, n);
    }


    std::tuple<unsigned, unsigned> offset_after_n_chars(handle& s, unsigned n, unsigned offset)
    {
      unsigned cnt = 0;
      while (offset < s.buffer.size() && cnt < n) {
        assert(s.buffer[offset] < 0x80 || ((s.buffer[offset] & 0xc0) != 0x80 && s.buffer[offset] < 0xf8));
        if (s.buffer[offset] < 0x80)
          offset += 1;
        else if (s.buffer[offset] < 0xe0)
          offset += 2;
        else if (s.buffer[offset] < 0xf0)
          offset += 3;
        else
          offset += 4;
        // No incomplete or invalid character should have been added to the buffer.
        assert(offset <= s.buffer.size());
        ++cnt;
      }
      return {offset, cnt};
    }


    void recompute_line_offset(handle& s, int r)
    {
      unsigned avail = s.term_cols - (r == 0 ? s.prompt_len : 0);
      s.line_offset.resize(r + 1);
      auto o = s.line_offset[r];
      while (o < s.buffer.size()) {
        auto [next, nchars] = offset_after_n_chars(s, avail, o);

        if (nchars < avail)
          break;
        s.line_offset.push_back(next);
        o = next;
        avail = s.term_cols;
      }
    }


    struct key {
      bool sym;
      int mod;
      long code;

      std::strong_ordering operator<=>(const key& k) const
      {
        if (k.mod == mod) {
          if (k.sym == sym)
            return k.code <=> code;
          return k.sym <=> sym;
        } else
          return k.mod <=> mod;
      }
    };

    using key_function = bool (*)(handle&);


    bool cb_beginning_of_line(handle& s)
    {
      if (s.offset != 0) {
        s.pos_x = s.prompt_len;
        s.pos_y = 0;
        s.offset = 0;
        move_to(s, s.pos_x, s.pos_y);
      }
      return false;
    }

    bool cb_end_of_line(handle& s)
    {
      if (s.offset != s.buffer.size()) {
        s.pos_y = s.line_offset.size() - 1;
        s.requested_pos_x = s.pos_x = (s.pos_y == 0 ? s.prompt_len : 0) + ::u8_strnlen(s.buffer.data() + s.line_offset[s.pos_y], s.buffer.size() - s.line_offset[s.pos_y]);
        s.offset = s.buffer.size();
        move_to(s, s.pos_x, s.pos_y);
      }
      return false;
    }

    bool cb_insert(handle& s)
    {
      s.insert = ! s.insert;
      return false;
    }

    bool cb_enter([[maybe_unused]] handle& s)
    {
      return true;
    }

    bool cb_backward_char(handle& s)
    {
      if (s.offset > 0) {
        ucs4_t uc;
        auto prevp = ::u8_prev(&uc, s.buffer.data() + s.offset, s.buffer.data());
        assert(prevp != nullptr);
        s.offset = prevp - s.buffer.data();
        if (s.pos_x == 0) {
          if (s.multiline) {
            assert(s.pos_y > 0);
            s.pos_x = s.term_cols - 1;
            s.pos_y -= 1;
          } else {
          }
        } else
          s.pos_x -= 1;
        s.requested_pos_x = s.pos_x;
        move_to(s, s.pos_x, s.pos_y);
      }
      return false;
    }

    bool cb_forward_char(handle& s)
    {
      if (s.offset < s.buffer.size()) {
        ucs4_t uc;
        s.offset += ::u8_mbtoucr(&uc, s.buffer.data() + s.offset, s.buffer.size() - s.offset);
        assert(uc != 0xfffd);
        if (s.pos_x + 1 == s.term_cols) {
          if (s.multiline) {
            assert(s.pos_y < s.line_offset.size());
            s.pos_x = 0;
            s.pos_y += 1;
          } else {
          }
        } else
          s.pos_x += 1;
        s.requested_pos_x = s.pos_x;
        move_to(s, s.pos_x, s.pos_y);
      }
      return false;
    }

    bool cb_previous_screen_line(handle& s)
    {
      if (s.pos_y > 0) {
        if (s.pos_y > 1 || s.requested_pos_x >= s.prompt_len) {
          s.pos_y -= 1;
          std::tie(s.offset, s.pos_x) = offset_after_n_chars(s, s.requested_pos_x - (s.pos_y == 0 ? s.prompt_len : 0), s.line_offset[s.pos_y]);
          if (s.pos_y == 0)
            s.pos_x += s.prompt_len;
          move_to(s, s.pos_x, s.pos_y);
        }
      }
      return false;
    }

    bool cb_next_screen_line(handle& s)
    {
      if (s.pos_y + 1 < s.line_offset.size()) {
        s.pos_y += 1;
        s.requested_pos_x = s.pos_x;
        std::tie(s.offset, s.pos_x) = offset_after_n_chars(s, s.requested_pos_x, s.line_offset[s.pos_y]);
        move_to(s, s.pos_x, s.pos_y);
      }
      return false;
    }

    bool cb_backspace(handle& s)
    {
      if (s.offset > 0) {
        auto old_offset = s.offset;
        (void) cb_backward_char(s);
        assert(s.offset != old_offset);
        s.buffer.erase(s.buffer.begin() + s.offset, s.buffer.begin() + old_offset);
        s.nchars -= 1;
        recompute_line_offset(s, s.pos_y);
        char movbuf[40];
        size_t nmovbuf = move_to_buf(movbuf, sizeof(movbuf), s, s.pos_x, s.pos_y);
        // clang-format off
        std::array<iovec, 3> iov
        {
          { {s.buffer.data() + s.offset, s.buffer.size() - s.offset},
            {const_cast<char*>(" "), 1},
            {movbuf, nmovbuf}
          }
        };
        // clang-format on
        ::writev(s.fd, iov.data(), iov.size());
        s.requested_pos_x = s.pos_x;
      }
      return false;
    }

    bool cb_delete(handle& s)
    {
      if (s.offset < s.buffer.size()) {
        ucs4_t _;
        auto n = ::u8_mbtoucr(&_, s.buffer.data() + s.offset, s.buffer.size() - s.offset);
        assert(n > 0);
        s.buffer.erase(s.buffer.begin() + s.offset, s.buffer.begin() + s.offset + n);
        s.nchars -= 1;
        recompute_line_offset(s, s.pos_y);
        char movbuf[40];
        size_t nmovbuf = move_to_buf(movbuf, sizeof(movbuf), s, s.pos_x, s.pos_y);
        // clang-format off
        std::array<iovec, 3> iov
        {
          { {s.buffer.data() + s.offset, s.buffer.size() - s.offset},
            {const_cast<char*>(" "), 1},
            {movbuf, nmovbuf}
          }
        };
        // clang-format on
        ::writev(s.fd, iov.data(), iov.size());
        s.requested_pos_x = s.pos_x;
      }
      return false;
    }

    bool cb_backward_word(handle& s)
    {
      if (s.offset > 0) {
        auto cat = uc_general_category_or(UC_LETTER, UC_NUMBER);
        ucs4_t uc1;
        auto p = ::u8_prev(&uc1, s.buffer.data() + s.offset, s.buffer.data());
        while (p > s.buffer.data()) {
          ucs4_t uc2;
          auto q = ::u8_prev(&uc2, p, s.buffer.data());
          if (::uc_is_general_category(uc1, cat) && ! ::uc_is_general_category(uc2, cat))
            break;
          p = q;
          uc1 = uc2;
        }

        s.offset = p - s.buffer.data();
        while (s.line_offset[s.pos_y] > s.offset) {
          assert(s.pos_y > 0);
          --s.pos_y;
        }
        s.pos_x = ::u8_mbsnlen(s.buffer.data() + s.line_offset[s.pos_y], s.offset - s.line_offset[s.pos_y]);
        if (s.pos_y == 0)
          s.pos_x += s.prompt_len;
        s.requested_pos_x = s.pos_x;
        move_to(s, s.pos_x, s.pos_y);
      }
      return false;
    }

    bool cb_forward_word(handle& s)
    {
      if (s.offset + 1 < s.buffer.size()) {
        auto cat = uc_general_category_or(UC_LETTER, UC_NUMBER);
        ucs4_t uc1;
        auto p = ::u8_next(&uc1, s.buffer.data() + s.offset);
        if (p < s.buffer.data() + s.buffer.size()) {
          auto q = ::u8_next(&uc1, p);
          while (q <= s.buffer.data() + s.buffer.size()) {
            if (q == s.buffer.data() + s.buffer.size()) {
              p = q;
              break;
            }
            ucs4_t uc2;
            auto r = ::u8_next(&uc2, q);
            if (::uc_is_general_category(uc1, cat) && ! ::uc_is_general_category(uc2, cat)) {
              p = q;
              break;
            }
            p = q;
            q = r;
            uc1 = uc2;
          }
        }

        s.offset = p - s.buffer.data();
        while (s.pos_y + 1 < s.line_offset.size() && s.offset >= s.line_offset[s.pos_y + 1])
          ++s.pos_y;
        s.pos_x = ::u8_mbsnlen(s.buffer.data() + s.line_offset[s.pos_y], s.offset - s.line_offset[s.pos_y]);
        if (s.pos_y == 0)
          s.pos_x += s.prompt_len;
        s.requested_pos_x = s.pos_x;
        move_to(s, s.pos_x, s.pos_y);
      }
      return false;
    }


    bool cb_unix_line_discard(handle& s)
    {
      if (s.offset > 0) {
        s.buffer.erase(s.buffer.begin(), s.buffer.begin() + s.offset);
        auto old_nlines = s.line_offset.size();
        s.pos_x = s.prompt_len;
        s.pos_y = 0;
        recompute_line_offset(s, 0);
        // clang-format off
        char movbuf[40];
        size_t nmovbuf = move_to_buf(movbuf, sizeof(movbuf), s, s.pos_x, s.pos_y);
        std::vector<iovec> iov
        {
          {movbuf, nmovbuf},
          {s.buffer.data(), s.buffer.size()},
          {const_cast<char*>("\e[K"), 3},
        };
        // clang-format on
        while (old_nlines-- > s.line_offset.size())
          iov.emplace_back(const_cast<char*>("\n\e[K"), 4);
        iov.emplace_back(movbuf, nmovbuf);
        ::writev(s.fd, iov.data(), iov.size());
      }
      return false;
    }


    bool cb_kill_line(handle& s)
    {
      if (s.offset < s.buffer.size()) {
        s.buffer.erase(s.buffer.begin() + s.offset, s.buffer.end());
        auto old_nlines = s.line_offset.size();
        recompute_line_offset(s, s.pos_y);
        // clang-format off
        std::vector<iovec> iov
        {
          {const_cast<char*>("\e[K"), 3},
        };
        // clang-format on
        if (old_nlines > s.line_offset.size()) {
          while (old_nlines-- > s.line_offset.size())
            iov.emplace_back(const_cast<char*>("\n\e[K"), 4);
          char movbuf[40];
          size_t nmovbuf = move_to_buf(movbuf, sizeof(movbuf), s, s.pos_x, s.pos_y);
          iov.emplace_back(movbuf, nmovbuf);
        }
        ::writev(s.fd, iov.data(), iov.size());
      }
      return false;
    }


    void show_empty_message(handle& s)
    {
      auto colon = std::format("\e[38;2;{};{};{}m", s.empty_message_fg.r, s.empty_message_fg.g, s.empty_message_fg.b);
      std::string coloff;
      if (s.text_default_fg != terminal::info::color{})
        coloff = std::format("\e[38;2;{};{};{}m", s.text_default_fg.r, s.text_default_fg.g, s.text_default_fg.b);
      else
        coloff = "\e[m";
      char movebuf[40];
      auto nmovebuf = move_to_buf(movebuf, sizeof(movebuf), s, s.pos_x, s.pos_y);
      // clang-format off
      std::array<iovec, 4> iov{
        {
          {colon.data(), colon.size()},
          {s.empty_message.data(), s.empty_message.size()},
          {coloff.data(), coloff.size()},
          {movebuf, static_cast<size_t>(nmovebuf) }},
      };
      // clang-format on
      ::writev(s.fd, iov.data(), iov.size());
    }


    // clang-format off
    std::map<key, key_function> key_map{
      {{false, ::TERMKEY_KEYMOD_CTRL, 'a'}, cb_beginning_of_line},
      {{true, 0, ::TERMKEY_SYM_HOME}, cb_beginning_of_line},
      {{false, ::TERMKEY_KEYMOD_CTRL, 'e'}, cb_end_of_line},
      {{true, 0, ::TERMKEY_SYM_END}, cb_end_of_line},
      {{true, 0, ::TERMKEY_SYM_INSERT}, cb_insert},
      {{true, 0, ::TERMKEY_SYM_ENTER}, cb_enter},
      {{true, 0, ::TERMKEY_SYM_LEFT}, cb_backward_char},
      {{true, 0, ::TERMKEY_SYM_RIGHT}, cb_forward_char},
      {{true, 0, ::TERMKEY_SYM_UP}, cb_previous_screen_line},
      {{true, 0, ::TERMKEY_SYM_DOWN}, cb_next_screen_line},
      {{true, 0, ::TERMKEY_SYM_BACKSPACE}, cb_backspace},
      {{true, 0, ::TERMKEY_SYM_DELETE}, cb_delete},
      {{false, ::TERMKEY_KEYMOD_ALT, 'b'}, cb_backward_word},
      {{false, ::TERMKEY_KEYMOD_ALT, 'f'}, cb_forward_word},
      {{false, ::TERMKEY_KEYMOD_CTRL, 'u'}, cb_unix_line_discard},
      {{false, ::TERMKEY_KEYMOD_CTRL, 'k'}, cb_kill_line},
    };
    // clang-format on


    bool on_key(handle& s, ::TermKeyKey key)
    {
      if (key.type == ::TERMKEY_TYPE_UNICODE) {
        if ((key.modifiers & (::TERMKEY_KEYMOD_ALT | ::TERMKEY_KEYMOD_CTRL)) == 0) {
          assert(s.offset <= s.buffer.size());
          uint8_t buf[8];
          auto l = ::u8_uctomb(buf, key.code.codepoint, sizeof(buf));
          auto to_print = l;

          if (s.buffer.empty() && ! s.empty_message.empty())
            ::write(s.fd, "\e[K", 3);

          if (s.insert || s.offset == s.buffer.size()) {
            s.buffer.insert(s.buffer.begin() + s.offset, buf, buf + l);
            s.nchars += 1;

            if (s.multiline) {
              // Recompute the affected line starts.
              [[maybe_unused]] auto old_nlines = s.line_offset.size();
              recompute_line_offset(s, s.pos_y);
              to_print = s.buffer.size() - s.offset;
              if (s.pos_x == 0 && s.pos_y > 0 && s.offset + l == s.buffer.size()) {
                // Terminal emulators remember when a line is continued after the last column, even if
                // this is not visible.  We already moved the cursor to the next line.  To achieve the
                // continuation we go back and write the last character of the previous line and the new
                // character together.
                move_to(s, s.term_cols, s.pos_y - 1);
                ucs4_t _;
                auto p = ::u8_prev(&_, s.buffer.data() + s.offset, s.buffer.data());
                ::write(s.fd, p, l + (s.buffer.data() + s.offset - p));
              } else
                ::write(s.fd, s.buffer.data() + s.offset, to_print);
              if (s.line_offset.size() > s.max_lines) {
                assert(s.line_offset.size() == s.max_lines + 1);
                s.max_lines = s.line_offset.size();
                if (s.initial_row + s.line_offset.size() - 1 + s.cur_frame_lines > s.term_rows) {
                  // Need to scroll.
                  assert(s.line_offset.size() - old_nlines == 1);
                  s.initial_row -= 1;
                  static const char SUrDL[] = "\e[S\r\e[1L";
                  ::write(s.fd, SUrDL, strlen(SUrDL));
                } else if (s.cur_frame_lines > 0) {
                  static const char nDL[] = "\n\e[1L";
                  ::write(s.fd, nDL, strlen(nDL));
                }
              }
            } else {
              if (s.initial_col + s.pos_x > std::max(1u, unsigned(0.9 * s.term_cols))) {
                auto nchars = std::max(1u, unsigned(0.1 * s.term_cols));
                auto [new_offset, nchars2] = offset_after_n_chars(s, nchars, s.line_offset[0]);
                if (new_offset > s.offset)
                  new_offset = s.offset;
                nchars2 = ::u8_mbsnlen(s.buffer.data(), new_offset);
                assert(nchars2 > 0);
                s.line_offset[0] = new_offset;
                move_to(s, 1, s.initial_row);
                static const char before[] = "«";
                ::write(s.fd, before, strlen(before));
                s.pos_x = 1 + ::u8_mbsnlen(s.buffer.data() + new_offset, s.offset - new_offset);
                std::tie(new_offset, nchars2) = offset_after_n_chars(s, s.term_cols - 1, new_offset);
                to_print = new_offset - s.line_offset[0];
                ::write(s.fd, s.buffer.data() + s.line_offset[0], to_print);
              } else {
                auto nchars = std::min(s.term_cols - (s.initial_col + s.pos_x), unsigned(s.buffer.size()));
                auto [new_offset, nchars2] = offset_after_n_chars(s, nchars, s.offset);
                to_print = new_offset - s.offset;
                ::write(s.fd, s.buffer.data() + s.offset, to_print);
              }
            }
          } else {
            ucs4_t ignore;
            auto l_old = ::u8_mbtoucr(&ignore, s.buffer.data() + s.offset, s.buffer.size() - s.offset);
            assert(ignore != 0xfffd);
            if (l_old != l) {
              int delta = l - l_old;
              if (delta < 0)
                s.buffer.insert(s.buffer.begin() + s.offset, 0, l - l_old);
              else
                s.buffer.erase(s.buffer.begin() + s.offset, s.buffer.begin() + s.offset + (l_old - l));

              // Adjust the later line offsets.
              std::for_each(s.line_offset.begin() + s.pos_y + 1, s.line_offset.end(), [](auto& n) { n += 1; });
            }
            std::copy_n(buf, l, s.buffer.begin() + s.offset);
            ::write(s.fd, s.buffer.data() + s.offset, l);
          }

          s.offset += l;
          s.requested_pos_x = s.pos_x += 1;
          if (s.pos_x == s.term_cols) {
            s.pos_x = 0;
            s.pos_y += 1;
            // Force moving the cursor.  Otherwise the cursor stays at the end of the line when the
            // new character is written into the last column.
            to_print = 2;
          }

          if (to_print > 1)
            move_to(s, s.pos_x, s.pos_y);
        } else if (auto cb = key_map.find({false, key.modifiers & (::TERMKEY_KEYMOD_ALT | ::TERMKEY_KEYMOD_SHIFT | ::TERMKEY_KEYMOD_CTRL), key.code.codepoint}); cb != key_map.end())
          return cb->second(s);
      } else if (key.type == ::TERMKEY_TYPE_KEYSYM) {
        auto was_empty = s.buffer.empty();
        if (auto cb = key_map.find({true, key.modifiers & (::TERMKEY_KEYMOD_ALT | ::TERMKEY_KEYMOD_SHIFT | ::TERMKEY_KEYMOD_CTRL), key.code.sym}); cb != key_map.end()) {
          auto res = cb->second(s);
          if (! was_empty && s.buffer.empty() && ! s.empty_message.empty()) [[unlikely]]
            show_empty_message(s);
          return res;
        }
      }

      return false;
    }


    std::tuple<bool, bool> handle_one(handle& s, ::epoll_event& epev)
    {
      if (epev.data.fd == s.tkfd) {
        ::termkey_advisereadable(s.tk);

        ::TermKeyKey key;
        ::TermKeyResult r;
        while ((r = ::termkey_getkey(s.tk, &key)) == ::TERMKEY_RES_KEY) {
          if (key.type == ::TERMKEY_TYPE_UNICODE && key.modifiers == ::TERMKEY_KEYMOD_CTRL) {
            if (key.code.codepoint == 'C' || key.code.codepoint == 'c' || (s.buffer.empty() && (key.code.codepoint == 'D' || key.code.codepoint == 'd')))
              return {true, true};
          }

          if (on_key(s, key))
            return {true, true};
        }

        if (r == ::TERMKEY_RES_EOF)
          return {true, true};
      } else if (epev.data.fd == s.sigfd) {
        ::signalfd_siginfo si;
        ::read(s.sigfd, &si, sizeof(si));
        std::tie(s.term_cols, s.term_rows) = update_winsize(s.fd);

        // TODO: query cursor position and also if necesary adjust what is visible
      } else
        return {false, false};

      return {true, false};
    }


    void setup_epoll(handle& s)
    {
      assert(s.term_state == state::closed || s.term_state == state::open);

      if (s.term_state == state::closed) {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGWINCH);
        if (::sigprocmask(SIG_BLOCK, &mask, &s.old_mask) != 0) [[unlikely]]
          // This really should never happen.
          ::error(EXIT_FAILURE, errno, "sigprocmask failed ?!");

        std::tie(s.term_cols, s.term_rows) = update_winsize(s.fd);

        s.sigfd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (s.sigfd == -1) [[unlikely]]
          // This really should never happen.
          ::error(EXIT_FAILURE, errno, "sigfd failed ?!");

        epoll_event epev;
        epev.events = EPOLLIN | EPOLLERR;
        epev.data.fd = s.tkfd;
        if (::epoll_ctl(s.epfd, EPOLL_CTL_ADD, s.tkfd, &epev) != 0) [[unlikely]] {
          if (errno == EPERM) {
            assert(s.tk == nullptr);
            throw std::filesystem::filesystem_error("cannot use file descriptor", std::make_error_code(std::errc::inappropriate_io_control_operation));
          } else [[unlikely]]
            ::error(EXIT_FAILURE, errno, "epoll_ctl failed");
        } else
          ::fcntl(s.fd, F_SETFL, ::fcntl(s.fd, F_GETFL) | O_NONBLOCK);

        epev.events = EPOLLIN | EPOLLERR;
        epev.data.fd = s.sigfd;
        if (::epoll_ctl(s.epfd, EPOLL_CTL_ADD, s.sigfd, &epev) != 0) [[unlikely]]
          ::error(EXIT_FAILURE, errno, "epoll_ctl failed");

        s.term_state = state::open;
      }
    }


    void cleanup_epoll(handle& s)
    {
      assert(s.term_state == state::open);

      if (! sigismember(&s.old_mask, SIGWINCH)) {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGWINCH);
        if (::sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) [[unlikely]]
          // This really should never happen.
          ::error(EXIT_FAILURE, errno, "sigprocmask failed ?!");
      }

      // Ignore errors.  Maybe someone else cleared all descriptors?
      (void) ::epoll_ctl(s.epfd, EPOLL_CTL_DEL, s.tkfd, nullptr);
      (void) ::epoll_ctl(s.epfd, EPOLL_CTL_DEL, s.sigfd, nullptr);

      s.term_state = state::closed;
    }


    void finalize(handle& s)
    {
      if (s.text_default_fg != terminal::info::color{})
        ::write(s.fd, "\e[m", 3);

      if (s.osc133)
        ::write(s.fd, osc133_C, strlen(osc133_C));

      cleanup_epoll(s);
    }


    void leave(handle& s)
    {
      std::array<iovec, 7> iov;
      int niov = 0;

      char movbuf1[40];
      char movbuf2[40];
      std::string frame;
      if ((s.fl & handle::flags::frame) == handle::flags::frame_line && s.frame_highlight_fg != s.info->default_foreground) {
        // Undo the frame highlighting.
        for (size_t i = 0; i < s.term_cols; ++i)
          frame.append("─");
        size_t nmovbuf1 = move_to_buf(movbuf1, sizeof(movbuf1), s, 0, -1);
        iov[niov++] = {movbuf1, nmovbuf1};
        iov[niov++] = {frame.data(), frame.size()};
        size_t nmovbuf2 = move_to_buf(movbuf2, sizeof(movbuf2), s, 0, s.max_lines);
        iov[niov++] = {movbuf2, nmovbuf2};
        iov[niov++] = {frame.data(), frame.size()};
      }

      char movbuf3[40];
      if (s.buffer.empty() && ! s.empty_message.empty()) {
        size_t nmovbuf3 = move_to_buf(movbuf3, sizeof(movbuf3), s, s.pos_x, s.pos_y);
        iov[niov++] = {movbuf3, nmovbuf3};
        iov[niov++] = {const_cast<char*>("\e[K"), 3zu};
      }

      char movbuf4[40];
      size_t nmovbuf4 = move_to_buf(movbuf4, sizeof(movbuf4) - 1, s, s.term_cols - 1, ((s.fl & handle::flags::frame) == handle::flags::none ? s.line_offset.size() : s.max_lines) - 1 + s.cur_frame_lines);
      movbuf4[nmovbuf4++] = '\n';
      iov[niov++] = {movbuf4, nmovbuf4};

      ::writev(s.fd, iov.data(), niov);
    }


    void the_loop(handle& s)
    {
      s.prepare();

      std::array<::epoll_event, 1> epev;
      do {
        auto n = TEMP_FAILURE_RETRY(::epoll_wait(s.epfd, epev.data(), epev.size(), -1));
        assert(n > 0);
      } while (! std::get<1>(handle_one(s, epev[0])));

      leave(s);

      finalize(s);
    }


    void init_state(handle& s)
    {
      TERMKEY_CHECK_VERSION;

      if ((s.fl & handle::flags::frame) == handle::flags::frame_background) {
        // We use as foreground a slightly adjusted version of the default background colors.
        auto [fg, bg] = adjust_rgb(s.info->default_foreground, s.info->default_background, 32);
        s.frame_highlight_fg = bg;
        s.text_default_fg = fg;
        s.text_default_bg = bg;
      }

      s.osc133 = s.info->feature_set.contains(terminal::scroll_markers);

      s.term_state = state::closed;
    }

  } // anonymous namespace


  handle::handle(int fd_, flags fl_) : fd(fd_), fl(fl_), info(terminal::info::alloc(fd)), frame_highlight_fg(info->default_foreground), tk(::termkey_new(fd, 0)), tkfd(::termkey_get_fd(tk)), epfd(::epoll_create1(EPOLL_CLOEXEC)), extern_epfd(false)
  {
    if (epfd == -1) [[unlikely]]
      // This really should never happen.
      ::error(EXIT_FAILURE, errno, "epoll_create failed ?!");

    init_state(*this);
  }


  handle::handle(int epfd_, int fd_, flags fl_) : fd(fd_), fl(fl_), info(terminal::info::alloc(fd)), frame_highlight_fg(info->default_foreground), tk(::termkey_new(fd, 0)), tkfd(::termkey_get_fd(tk)), epfd(epfd_), extern_epfd(true)
  {
    init_state(*this);
  }


  handle::~handle()
  {
    if (term_state == state::open)
      cleanup_epoll(*this);

    ::close(sigfd);
    if (! extern_epfd)
      ::close(epfd);
    ::sigprocmask(SIG_SETMASK, &old_mask, nullptr);
    ::termkey_destroy(tk);
  }


  void handle::set_prompt(const char* s)
  {
    prompt = s;
  }


  void handle::set_prompt(const std::string& s)
  {
    prompt = s;
  }


  void handle::set_prompt(const std::string_view s)
  {
    prompt = std::string(s);
  }


  void handle::set_prompt(string_callback prompt_fct)
  {
    prompt = prompt_fct;
  }


  std::string_view handle::read()
  {
    the_loop(*this);

    return std::string_view(reinterpret_cast<char*>(buffer.data()), buffer.size());
  }


  void handle::prepare()
  {
    assert(term_state == state::open || term_state == state::closed);

    if (term_state == state::closed) {
      setup_epoll(*this);

      buffer.clear();

      // Move to the next line beginning.
      if (osc133)
        ::write(fd, osc133_L, strlen(osc133_L));
      else
        ::write(fd, "\r", 1);

      if ((fl & handle::flags::frame) != handle::flags::none) {
        std::string frame;

        if (frame_highlight_fg != info->default_foreground)
          std::format_to(std::back_inserter(frame), "\e[38;2;{};{};{}m", frame_highlight_fg.r, frame_highlight_fg.g, frame_highlight_fg.b);
        auto f = (fl & handle::flags::frame) == handle::flags::frame_line ? "─" : "\N{LOWER HALF BLOCK}";
        for (size_t i = 0; i < term_cols; ++i)
          frame.append(f);
        frame.append("\n\n");
        f = (fl & handle::flags::frame) == handle::flags::frame_line ? "─" : "\N{UPPER HALF BLOCK}";
        for (size_t i = 0; i < term_cols; ++i)
          frame.append(f);
        if (frame_highlight_fg != info->default_foreground)
          frame.append("\e[0m");
        frame.append("\e[1F");
        ::write(fd, frame.data(), frame.size());
        cur_frame_lines = 1;

        if (text_default_fg != terminal::info::color{}) {
          auto colsel = std::format("\e[38;2;{};{};{};48;2;{};{};{}m", text_default_fg.r, text_default_fg.g, text_default_fg.b, text_default_bg.r, text_default_bg.g, text_default_bg.b);
          ::write(fd, colsel.data(), colsel.size());
        }
      } else
        cur_frame_lines = 0;

      // Get current position.
      std::tie(initial_col, initial_row) = get_current_pos(tkfd);
      assert(initial_col == 1);

      offset = 0u;
      nchars = 0u;
      pos_x = 0u;
      pos_y = 0u;
      line_offset = {0u};

      prompt_len = 0u;

      // For interactive use.
      assert(buffer.empty());

      std::string prompt_str;
      if (std::holds_alternative<std::string>(prompt))
        prompt_str = std::get<std::string>(prompt);
      else
        prompt_str = std::get<handle::string_callback>(prompt)();

      prompt_len = nonescape_len(prompt_str);

      if (! prompt_str.empty()) {
        if (osc133)
          ::write(fd, osc133_A, strlen(osc133_A));
        ::write(fd, prompt_str.data(), prompt_str.size());
      }
      if (osc133)
        ::write(fd, osc133_B, strlen(osc133_B));

      pos_x = prompt_len;

      // Clear to end of line.  This also fills in the background color, if needed.
      ::write(fd, "\e[K", 3);

      if (! empty_message.empty()) {
        // Determine the foregroun color to use.  It should be darker than the usual.
        terminal::info::color fg;
        terminal::info::color bg;
        if ((fl & handle::flags::frame) != handle::flags::frame_background)
          std::tie(fg, bg) = adjust_rgb(info->default_foreground, info->default_background, 48);
        else
          std::tie(fg, bg) = adjust_rgb(text_default_fg, text_default_bg, 48);
        empty_message_fg = bg;

        show_empty_message(*this);
      }
    }
  }


  std::expected<std::string_view, bool> handle::process(::epoll_event& epev)
  {
    auto [handled, done] = handle_one(*this, epev);
    if (! done)
      return std::unexpected(handled);

    finalize(*this);

    return std::string_view(reinterpret_cast<char*>(buffer.data()), buffer.size());
  }

} // namespace nrl
