#include <utility>
#ifndef NRL_HH_
# define NRL_HH_ 1

# include <csignal> // IWYU pragma: keep
# include <cstdint>
# include <string>
# include <string_view>
# include <variant>
# include <vector>

# include <termkey.h>

# include "termdetect/termdetect.hh"


namespace nrl {

  enum struct fd_state { open, no_terminal, closed };

  struct state {
    enum struct flags {
      none = 0,
      frame_line = 1,
      frame_background = 2,
      frame = 3,
    };

    state(int fd_, flags fl_ = flags::none);
    state(const state&) = delete;
    state& operator=(const state&) = delete;
    ~state();

    using string_callback = const char* (*) ();

    void set_prompt(const char* s);
    void set_prompt(const std::string& s);
    void set_prompt(const std::string_view s);
    void set_prompt(string_callback prompt_fct);

    std::vector<uint8_t> buffer{};
    std::vector<unsigned> line_offset{0};
    size_t filled = 0;
    size_t returned = 0;
    std::variant<std::string, string_callback> prompt{""};

    int fd;
    flags fl;
    fd_state term_state = fd_state::open;
    std::shared_ptr<terminal::info> info;

    unsigned term_rows = 0;
    unsigned term_cols = 0;
    unsigned cur_frame_lines = 0;
    terminal::info::color frame_highlight_fg{};

    TermKey* tk;
    int tkfd;

    sigset_t old_mask{};
    int sigfd = -1;

    int epfd = -1;

    // True if not scrolling but multi-line input is requested.
    bool multiline = true;
    // True if insert mode, false if overwrite.
    bool insert = true;
    // Use OSC 133 semantic prompts.
    bool osc133 = true;

    // Up-to-date in read calls.
    unsigned initial_col = 0;
    unsigned initial_row = 0;
    unsigned offset = 0;
    unsigned nchars = 0;
    unsigned pos_x = 0;
    unsigned requested_pos_x = 0;
    unsigned pos_y = 0;
    unsigned first = 0; // When not multiline, offset of the first character shown.
    unsigned prompt_len = 0;

    friend std::string_view read(state&);
  };


  inline state::flags operator&(state::flags l, state::flags r)
  {
    return static_cast<state::flags>(std::to_underlying(l) & std::to_underlying(r));
  }
  inline state::flags operator|(state::flags l, state::flags r)
  {
    return static_cast<state::flags>(std::to_underlying(l) | std::to_underlying(r));
  }


  std::string_view read(state& c);

} // namespace nrl

#endif // nrl.hh
