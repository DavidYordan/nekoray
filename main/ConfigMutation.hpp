#pragma once

namespace NekoGui_ConfigMutation {
    class Guard {
    public:
        explicit Guard(bool wait);
        ~Guard();

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

        [[nodiscard]] bool acquired() const;

    private:
        bool locked = false;
    };
} // namespace NekoGui_ConfigMutation
