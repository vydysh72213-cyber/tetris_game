#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <poll.h>
#include <random>
#include <string>
#include <termios.h>
#include <sys/select.h>
#include <unistd.h>

namespace {

constexpr int kWidth = 10;
constexpr int kHeight = 20;
using Shape = std::array<std::string, 4>;
using Board = std::array<std::array<int, kWidth>, kHeight>;

const std::array<Shape, 7> kShapes{{
    Shape{{"....", "####", "....", "...."}}, // I
    Shape{{".##.", ".##.", "....", "...."}}, // O
    Shape{{".#..", "###.", "....", "...."}}, // T
    Shape{{".##.", "##..", "....", "...."}}, // S
    Shape{{"##..", ".##.", "....", "...."}}, // Z
    Shape{{"#...", "###.", "....", "...."}}, // J
    Shape{{"..#.", "###.", "....", "...."}}  // L
}};

class TerminalMode {
public:
    TerminalMode() {
        if (tcgetattr(STDIN_FILENO, &original_) == 0) {
            termios raw = original_;
            raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            active_ = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
        }
        std::cout << "\033[?25l\033[2J\033[H";
        std::cout.flush();
    }

    ~TerminalMode() {
        if (active_) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
        }
        std::cout << "\033[0m\033[?25h\033[2J\033[H";
        std::cout.flush();
    }

    TerminalMode(const TerminalMode&) = delete;
    TerminalMode& operator=(const TerminalMode&) = delete;

private:
    termios original_{};
    bool active_ = false;
};

struct Piece {
    int type = 0;
    int x = 3;
    int y = 0;
    Shape shape{};
};

class Game {
public:
    Game() : rng_(std::random_device{}()), nextType_(randomPieceType()) {
        spawn();
    }

    void run() {
        using Clock = std::chrono::steady_clock;
        auto previous = Clock::now();
        auto lastDraw = previous;
        std::chrono::milliseconds accumulated{0};
        bool running = true;

        while (running) {
            const auto now = Clock::now();
            accumulated += std::chrono::duration_cast<std::chrono::milliseconds>(now - previous);
            previous = now;

            char key = readKey();
            if (key != '\0') {
                running = handleKey(key);
            }

            if (!paused_ && !gameOver_) {
                const int interval = std::max(100, 700 - (level_ - 1) * 55);
                while (accumulated.count() >= interval) {
                    accumulated -= std::chrono::milliseconds(interval);
                    if (!stepDown()) {
                        break;
                    }
                }
            } else {
                accumulated = std::chrono::milliseconds(0);
            }

            if (now - lastDraw >= std::chrono::milliseconds(45)) {
                draw();
                lastDraw = now;
            }
            usleep(10000);
        }
    }

private:
    Board board_{};
    Piece current_{};
    std::mt19937 rng_;
    int nextType_;
    int score_ = 0;
    int lines_ = 0;
    int level_ = 1;
    bool paused_ = false;
    bool gameOver_ = false;

    int randomPieceType() {
        std::uniform_int_distribution<int> pieceType(0, 6);
        return pieceType(rng_);
    }

    static Shape rotateClockwise(const Shape& shape) {
        Shape rotated{{"....", "....", "....", "...."}};
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                rotated[col][3 - row] = shape[row][col];
            }
        }
        return rotated;
    }

    bool fits(const Shape& shape, int x, int y) const {
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                if (shape[row][col] != '#') {
                    continue;
                }
                const int boardX = x + col;
                const int boardY = y + row;
                if (boardX < 0 || boardX >= kWidth || boardY >= kHeight) {
                    return false;
                }
                if (boardY >= 0 && board_[boardY][boardX] != 0) {
                    return false;
                }
            }
        }
        return true;
    }

    void spawn() {
        current_ = Piece{};
        current_.type = nextType_;
        current_.shape = kShapes[current_.type];
        current_.x = 3;
        current_.y = 0;
        nextType_ = randomPieceType();
        if (!fits(current_.shape, current_.x, current_.y)) {
            gameOver_ = true;
        }
    }

    void lockPiece() {
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                if (current_.shape[row][col] != '#') {
                    continue;
                }
                const int boardX = current_.x + col;
                const int boardY = current_.y + row;
                if (boardY < 0) {
                    gameOver_ = true;
                    continue;
                }
                board_[boardY][boardX] = current_.type + 1;
            }
        }

        clearLines();
        if (!gameOver_) {
            spawn();
        }
    }

    void clearLines() {
        int cleared = 0;
        for (int row = kHeight - 1; row >= 0; --row) {
            bool full = true;
            for (int col = 0; col < kWidth; ++col) {
                if (board_[row][col] == 0) {
                    full = false;
                    break;
                }
            }
            if (full) {
                ++cleared;
                for (int moveRow = row; moveRow > 0; --moveRow) {
                    board_[moveRow] = board_[moveRow - 1];
                }
                board_[0].fill(0);
                ++row;
            }
        }

        if (cleared > 0) {
            constexpr std::array<int, 5> points{{0, 100, 300, 500, 800}};
            score_ += points[cleared] * level_;
            lines_ += cleared;
            level_ = lines_ / 10 + 1;
        }
    }

    bool stepDown() {
        if (fits(current_.shape, current_.x, current_.y + 1)) {
            ++current_.y;
        } else {
            lockPiece();
        }
        return !gameOver_;
    }

    void hardDrop() {
        int distance = 0;
        while (fits(current_.shape, current_.x, current_.y + 1)) {
            ++current_.y;
            ++distance;
        }
        score_ += distance * 2;
        lockPiece();
    }

    void tryRotate() {
        const Shape rotated = rotateClockwise(current_.shape);
        if (fits(rotated, current_.x, current_.y)) {
            current_.shape = rotated;
        }
    }

    bool handleKey(char key) {
        if (key == 'q' || key == 'Q') {
            return false;
        }
        if (gameOver_) {
            if (key == 'r' || key == 'R') {
                board_ = Board{};
                score_ = lines_ = 0;
                level_ = 1;
                gameOver_ = false;
                paused_ = false;
                spawn();
            }
            return true;
        }
        if (key == 'p' || key == 'P') {
            paused_ = !paused_;
            return true;
        }
        if (paused_) {
            return true;
        }

        switch (key) {
        case 'a':
        case 'A':
        case 'h':
        case 'H':
            if (fits(current_.shape, current_.x - 1, current_.y)) {
                --current_.x;
            }
            break;
        case 'd':
        case 'D':
        case 'l':
        case 'L':
            if (fits(current_.shape, current_.x + 1, current_.y)) {
                ++current_.x;
            }
            break;
        case 's':
        case 'S':
        case 'j':
        case 'J':
            if (!stepDown()) {
                return true;
            }
            break;
        case 'w':
        case 'W':
        case 'k':
        case 'K':
        case 'x':
        case 'X':
            tryRotate();
            break;
        case ' ':
            hardDrop();
            break;
        case '\x1b':
            return true;
        }
        return true;
    }

    static char readKey() {
        pollfd input{STDIN_FILENO, POLLIN, 0};
        if (poll(&input, 1, 0) <= 0 || (input.revents & POLLIN) == 0) {
            return '\0';
        }

        char key = '\0';
        if (read(STDIN_FILENO, &key, 1) != 1) {
            return '\0';
        }
        if (key != '\x1b') {
            return key;
        }

        input.revents = 0;
        if (poll(&input, 1, 30) <= 0 || (input.revents & POLLIN) == 0) {
            return key;
        }

        char sequenceStart = '\0';
        if (read(STDIN_FILENO, &sequenceStart, 1) != 1 || sequenceStart != '[') {
            return key;
        }

        input.revents = 0;
        if (poll(&input, 1, 30) <= 0 || (input.revents & POLLIN) == 0) {
            return key;
        }
        char direction = '\0';
        if (read(STDIN_FILENO, &direction, 1) == 1) {
            switch (direction) {
            case 'A': return 'w';
            case 'B': return 's';
            case 'C': return 'd';
            case 'D': return 'a';
            }
        }
        return key;
    }

    void draw() const {
        std::cout << "\033[2J\033[H";
        std::cout << "        T E T R I S\n";
        std::cout << "  +";
        for (int col = 0; col < kWidth; ++col) {
            std::cout << "--";
        }
        std::cout << "+       SCORE: " << score_ << "\n";

        for (int row = 0; row < kHeight; ++row) {
            std::cout << "  |";
            for (int col = 0; col < kWidth; ++col) {
                int cell = board_[row][col];
                if (!gameOver_) {
                    for (int pieceRow = 0; pieceRow < 4; ++pieceRow) {
                        for (int pieceCol = 0; pieceCol < 4; ++pieceCol) {
                            if (current_.shape[pieceRow][pieceCol] == '#' &&
                                current_.x + pieceCol == col &&
                                current_.y + pieceRow == row) {
                                cell = current_.type + 1;
                            }
                        }
                    }
                }
                if (cell == 0) {
                    std::cout << "  ";
                } else {
                    std::cout << "\033[" << (31 + (cell - 1) % 7) << "m[]\033[0m";
                }
            }
            std::cout << "|";
            if (row == 1) {
                std::cout << "       LINES: " << lines_;
            } else if (row == 3) {
                std::cout << "       LEVEL: " << level_;
            } else if (row == 6) {
                std::cout << "  A/D or arrows: move";
            } else if (row == 7) {
                std::cout << "  W or Up: rotate";
            } else if (row == 8) {
                std::cout << "  S or Down: soft drop";
            } else if (row == 9) {
                std::cout << "  Space: hard drop";
            } else if (row == 10) {
                std::cout << "  P: pause   Q: quit";
            } else if (row == 12) {
                std::cout << "  NEXT:";
            } else if (row >= 13 && row <= 16) {
                std::cout << "  ";
                const Shape& nextShape = kShapes[nextType_];
                for (int col = 0; col < 4; ++col) {
                    if (nextShape[row - 13][col] == '#') {
                        std::cout << "\033[" << (31 + nextType_ % 7) << "m[]\033[0m";
                    } else {
                        std::cout << "  ";
                    }
                }
            }
            std::cout << "\n";
        }

        std::cout << "  +";
        for (int col = 0; col < kWidth; ++col) {
            std::cout << "--";
        }
        std::cout << "+\n";

        if (gameOver_) {
            std::cout << "\n       GAME OVER - press R to restart or Q to quit\n";
        } else if (paused_) {
            std::cout << "\n                 PAUSED\n";
        } else {
            std::cout << "\n";
        }
        std::cout.flush();
    }
};

} // namespace

int main() {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::cerr << "Run Tetris in a terminal window.\n";
        return EXIT_FAILURE;
    }

    TerminalMode terminal;
    Game game;
    game.run();
    return EXIT_SUCCESS;
}