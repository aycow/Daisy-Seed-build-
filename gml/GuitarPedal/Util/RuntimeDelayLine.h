#pragma once

// Adapted from Electro-Smith DaisySP DelayLine and BKShepherd's runtime-sized
// variant. Copyright (c) 2020 Electrosmith, Corp. Used under the MIT-style
// license published at https://opensource.org/licenses/MIT.

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace daisysp_modified
{
template <typename T>
class DelayLine
{
  public:
    DelayLine()
    : line_(nullptr), max_size_(0), write_ptr_(0), delay_(1), frac_(0.0f)
    {
    }

    void Init(T* line, size_t max_size)
    {
        line_     = line;
        max_size_ = max_size;
        Reset();
    }

    void Reset()
    {
        if(line_ == nullptr || max_size_ == 0)
            return;
        for(size_t i = 0; i < max_size_; ++i)
            line_[i] = T(0);
    }

    void SetDelay(size_t delay)
    {
        if(max_size_ == 0)
        {
            delay_ = 0;
            frac_  = 0.0f;
            return;
        }
        delay_ = delay < max_size_ ? delay : max_size_ - 1;
        frac_  = 0.0f;
    }

    void SetDelay(float delay)
    {
        if(max_size_ == 0)
        {
            delay_ = 0;
            frac_  = 0.0f;
            return;
        }
        const int32_t whole = static_cast<int32_t>(delay);
        frac_               = delay - static_cast<float>(whole);
        const size_t safe   = static_cast<size_t>(std::max(whole, int32_t(0)));
        delay_              = safe < max_size_ ? safe : max_size_ - 1;
    }

    T Read() const
    {
        if(line_ == nullptr || max_size_ == 0)
            return T(0);
        const T a = line_[(write_ptr_ + delay_) % max_size_];
        const T b = line_[(write_ptr_ + delay_ + 1) % max_size_];
        return a + (b - a) * frac_;
    }

    void Write(T sample)
    {
        if(line_ == nullptr || max_size_ == 0)
            return;
        line_[write_ptr_] = sample;
        write_ptr_        = (write_ptr_ - 1 + max_size_) % max_size_;
    }

  private:
    T*     line_;
    size_t max_size_;
    size_t write_ptr_;
    size_t delay_;
    float  frac_;
};
} // namespace daisysp_modified
