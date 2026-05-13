#include <memory>
#ifndef NRL_HH_
# define NRL_HH_ 1

# include <csignal> // IWYU pragma: keep
# include <cstdint>
# include <expected>
# include <string>
# include <string_view>
# include <utility>
# include <variant>
# include <vector>

# include <sys/epoll.h>

# include <termkey.h>

# include "termdetect/termdetect.hh"


namespace nrl {

  enum struct state { invalid, open, closed, archived };

  struct handle {
    /// Screen management interface for handling scrolling and line preservation
    struct screen_manager {
      /// Get number of rows after textbox that must be preserved
      /// @return Number of rows that should not be scrolled off screen
      virtual unsigned get_fixed_rows() const = 0;

      /// Insert or delete lines at current cursor position
      /// @param delta Number of lines to insert (positive) or delete (negative)
      virtual void adjust_lines(int delta) = 0;

      virtual ~screen_manager() = default;
    };

    /// Default screen manager implementation
    struct default_screen_manager final : screen_manager {
      int fd;

      explicit default_screen_manager(int fd_) : fd{fd_} {}

      /// Returns 0 to match current behavior (no fixed rows)
      unsigned get_fixed_rows() const override;

      /// Insert or delete lines using CSI escape sequences
      void adjust_lines(int delta) override;
    };

    enum struct flags {
      none = 0,
      frame_line = 1,
      frame_background = 2,
      frame = 3,
    };

    handle(int fd_, flags fl_ = flags::none, std::shared_ptr<terminal::info> info_ = {});
    handle(int epfd_, int fd_, flags fl_ = flags::none, std::shared_ptr<terminal::info> info_ = {});
    handle(const handle&) = delete;
    handle& operator=(const handle&) = delete;
    ~handle();

    std::string_view read();

    void prepare();
    void prepare(const std::vector<std::string>& select, bool multi_ = false);
    std::expected<std::string_view, bool> process(::epoll_event& epev);
    void redraw();

    void adjust_start(int delta) { initial_row += delta; }

    bool active_p() const { return term_state == state::closed || term_state == state::open; }

    using string_callback = const char* (*) ();

    void set_prompt(std::string&& s);
    void set_prompt(const char* s) { return set_prompt(std::string_view{s}); }
    void set_prompt(const std::string_view s);
    void set_prompt(string_callback prompt_fct);

    void set_answer(std::string&& s);
    void set_answer(const char* s) { return set_answer(std::string_view{s}); }
    void set_answer(const std::string_view s);
    void set_answer(string_callback prompt_fct);

    std::vector<uint8_t> buffer{};
    std::vector<unsigned> line_offset{0};
    size_t filled = 0;
    size_t returned = 0;
    size_t max_lines = 1;
    std::variant<std::monostate, std::string, string_callback> prompt{};
    std::variant<std::monostate, std::string, string_callback> answer{};

    std::string empty_message{};
    terminal::info::color empty_message_fg{};

    std::vector<std::string> select_options{};
    size_t select_idx = 0;
    std::set<size_t> selected{};
    bool multi = false;
    inline static constexpr char select_sep[] = "\N{NO-BREAK SPACE}&\N{NO-BREAK SPACE}";

    std::string& get_empty_message() { return select_options.empty() ? empty_message : select_options.front(); }

    void set_screen_manager(screen_manager* mgr);

    int fd;
    flags fl;
    state term_state = state::invalid;
    std::shared_ptr<terminal::info> info;

    unsigned term_rows = 0;
    unsigned term_cols = 0;
    unsigned cur_frame_lines = 0;
    terminal::info::color frame_highlight_fg{};
    terminal::info::color text_default_fg{};
    terminal::info::color text_default_bg{};
    std::string colsel{};

    TermKey* tk;
    int tkfd;

    sigset_t old_mask{};
    int sigfd = -1;

    int epfd;
    bool extern_epfd;

    // True if not scrolling but multi-line input is requested.
    bool multiline = true;
    // True if insert mode, false if overwrite.
    bool insert = true;
    // Use OSC 133 semantic prompts.
    bool osc133 = false;

    // Up-to-date in read calls.
    unsigned initial_col = 0;
    unsigned initial_row = 0;
    unsigned offset = 0;
    unsigned pos_x = 0;
    unsigned requested_pos_x = 0;
    unsigned pos_y = 0;
    unsigned first = 0; // When not multiline, offset of the first character shown.
    std::string prompt_str{};
    unsigned prompt_len = 0;

    default_screen_manager default_scr_mgr;
    screen_manager* scr_mgr = &default_scr_mgr;

    friend std::string_view read(handle&);

  private:
    void prepare_();
  };


  inline handle::flags operator&(handle::flags l, handle::flags r)
  {
    return static_cast<handle::flags>(std::to_underlying(l) & std::to_underlying(r));
  }
  inline handle::flags operator|(handle::flags l, handle::flags r)
  {
    return static_cast<handle::flags>(std::to_underlying(l) | std::to_underlying(r));
  }

} // namespace nrl

#endif // nrl.hh
