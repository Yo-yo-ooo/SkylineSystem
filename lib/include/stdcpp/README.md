# Complete C++ Standard Library Headers Status

---

## I. Language Support

| Header | Status | Complexity | Dependencies | Remarks |
| :--- | :--- | :--- | :--- | :--- |
| `<cstddef>` | **Done** | Very Low | None | Provides `size_t`, `ptrdiff_t`, `nullptr_t`, `max_align_t`, `offsetof`. |
| `<cstdint>` | **Done** | Very Low | None | Fixed-width integers (`int32_t`, `uint64_t`, etc.). |
| `<climits>` | **Done** | Very Low | None | Integer limits (`INT_MAX`, `LLONG_MAX`, etc.). |
| `<cfloat>` | **Done** | Very Low | None | Floating‑point limits (`FLT_MAX`, `DBL_EPSILON`, etc.). |
| `<cstdalign>` | **Done** | Very Low | None | Alignment macros (`alignof`, `alignas` compatibility). |
| `<cstdarg>` | **Done** | Very Low | None | Variable arguments (`va_list`, `va_start`, `va_end`). |
| `<cstdbool>` | **Done** | Very Low | None | Boolean macros (`true`, `false`) – mostly for C compatibility. |
| `<new>` | **Done** | High | `<cstddef>`, `<cstdlib>` | Implements `operator new/delete`, `bad_alloc`, `nothrow`, aligned allocation. |
| `<type_traits>` | **Done** | High | `<cstddef>`, `<cstdint>` | **Most important header** – foundation for all generic code. |
| `<initializer_list>` | **Done** | Very Low | None | Compiler‑built‑in support for `initializer_list<T>`. |

---

## II. Utilities

| Header | Status | Complexity | Dependencies | Remarks |
| :--- | :--- | :--- | :--- | :--- |
| `<utility>` | **Done** | Medium | `<type_traits>` | `std::move`, `std::forward`, `std::pair`, `std::index_sequence`. |
| `<tuple>` | **Done** | High | `<type_traits>`, `<utility>` | Variadic templates – `std::tuple`, `std::apply`. |
| `<functional>` | **Done** | Very High | `<type_traits>`, `<utility>`, `<tuple>` | `std::function`, `std::bind`, `std::invoke`, `std::reference_wrapper` etc. |
| `<memory>` | **Done** | High | `<type_traits>`, `<utility>`, `<new>` | `std::allocator`, `std::unique_ptr`, `std::shared_ptr`, `std::addressof`, `std::to_address`. |
| `<scoped_allocator>` | Deferred | High | `<memory>` | Nested allocators – rarely needed in kernel. |
| `<any>` | Deferred | Medium | `<type_traits>` | Type‑safe container – not commonly used in kernel. |

---

## III. Iterators & Algorithms

| Header | Status | Complexity | Dependencies | Remarks |
| :--- | :--- | :--- | :--- | :--- |
| `<iterator>` | **Done** | Very High | `<cstddef>`, `<type_traits>` | Iterator traits, `reverse_iterator`, inserters, C++20/23 components. |
| `<algorithm>` | **Done** | High | `<iterator>`, `<utility>` | `std::sort`, `std::find`, `std::copy`, `std::minmax` etc. **Large volume**. |
| `<numeric>` | Deferred | Medium | `<iterator>` | `std::accumulate`, `std::inner_product`, etc. |
| `<ranges>` | Deferred | Very High | `<algorithm>` | C++20 ranges views & adaptors – extremely complex. |
| `<execution>` | Deferred | Very High | `<algorithm>` | Parallel execution policies – depends on threading. |

---

## IV. Containers

| Header | Status | Complexity | Dependencies | Remarks |
| :--- | :--- | :--- | :--- | :--- |
| `<array>` | **Done** | Low | `<cstddef>` | Static array wrapper – simple. |
| `<vector>` | **Done** | Medium | `<memory>`, `<iterator>` | **Core container** – highest priority. |
| `<deque>` | **Done** | High | `<memory>`, `<iterator>` | Double‑ended queue – non‑trivial implementation. |
| `<list>` | **Done** | Medium | `<memory>`, `<iterator>` | Doubly‑linked list. |
| `<forward_list>` | **Done** | Medium | `<memory>`, `<iterator>` | Singly‑linked list. |
| `<map>` | **Done** | High | `<memory>`, `<iterator>`, `<utility>` | Red‑black tree – requires tree implementation. |
| `<set>` | **Done** | High | `<memory>`, `<iterator>`, `<utility>` | Red‑black tree. |
| `<unordered_map>` | **Done** | High | `<memory>`, `<iterator>`, `<functional>` | Hash table – requires hash functions. |
| `<unordered_set>` | **Done** | High | `<memory>`, `<iterator>`, `<functional>` | Hash table. |
| `<queue>` | **Done** | Low | `<deque>` or `<vector>` | Adaptor – depends on underlying container. |
| `<stack>` | **Done** | Low | `<deque>` or `<vector>` | Adaptor. |
| `<span>` | **Done** | Medium | `<cstddef>` | C++20 contiguous view – simple but requires compiler support. |

---

## V. Strings

| Header | Status | Complexity | Dependencies | Remarks |
| :--- | :--- | :--- | :--- | :--- |
| `<string>` | **Done** | High | `<memory>`, `<iterator>`, `<algorithm>` | `std::string` with SSO. **Core container** – high priority. |
| `<string_view>` | Deferred | Low | `<cstddef>` | C++17 read‑only string view – relatively simple. |

---

## VI. Streams & I/O

| Header | Status | Complexity | Dependencies | Remarks |
| :--- | :--- | :--- | :--- | :--- |
| `<ios>` | Deferred | Very High | `<locale>` | Base stream classes – depends on locales. |
| `<iostream>` | Deferred | Very High | `<ios>`, `<streambuf>` | Standard I/O streams – usually replaced by `kprintf` in kernel. |
| `<fstream>` | Deferred | Very High | `<iostream>`, `<filesystem>` | File streams – depends on filesystem. |
| `<sstream>` | Deferred | High | `<iostream>` | String streams – can be implemented later. |
| `<streambuf>` | Deferred | Very High | `<ios>`, `<locale>` | Stream buffers – very complex. |
| `<iomanip>` | Deferred | Medium | `<ios>` | Formatters like `setw`, `setprecision`. |
| `<cstdio>` | To Be Implemented | Low | None | C I/O (`printf`, `scanf` etc.) – often interfaced with `kprintf`. |
| `<cinttypes>` | Deferred | Low | `<cstdint>` | Extended integer formatting macros. |

---

## VII. Numerics

| Header | Status | Complexity | Dependencies | Remarks |
| :--- | :--- | :--- | :--- | :--- |
| `<complex>` | Deferred | Medium | None | Complex numbers – rarely used. |
| `<random>` | Deferred | High | `<type_traits>` | Random generators (`mt19937`, distributions). |
| `<ratio>` | Deferred | Medium | `<cstdint>` | Compile‑time rationals (C++11). |
| `<chrono>` | Deferred | High | `<ratio>`, `<cstdint>` | Time utilities – depends on system clock. |
| `<numbers>` | To Be Implemented | Very Low | None | C++20 mathematical constants (`pi`, `e`, `sqrt2` etc.) – simple. |
| `<cmath>` | Deferred | Medium | None | Math functions – may require FPU support. |
| `<cstdlib>` | To Be Implemented | Low | None | C library functions (`atoi`, `abs`, `qsort`, etc.). |

---

## VIII. Concurrency

| Header | Status | Complexity | Dependencies | Remarks |
| :--- | :--- | :--- | :--- | :--- |
| `<atomic>` | **Done** | High | `<cstdint>`, `<type_traits>` | Atomic operations – requires compiler builtins (`__atomic_*`). |
| `<thread>` | Deferred | High | `<chrono>`, `<mutex>` | Thread support – depends on your scheduler and `launch` syscall. |
| `<mutex>` | Deferred | Medium | `<thread>`, `<chrono>` | Mutexes, recursive locks. |
| `<condition_variable>` | Deferred | High | `<mutex>` | Condition variables. |
| `<future>` | Deferred | High | `<thread>`, `<functional>` | `std::future`, `std::promise` – depends on task scheduling. |

---

## IX. Filesystem

| Header | Status | Complexity | Dependencies | Remarks |
| :--- | :--- | :--- | :--- | :--- |
| `<filesystem>` | Deferred | Very High | `<chrono>`, `<iterator>` | Path operations, directory traversal – depends on VFS. |

---

## X. Miscellaneous

| Header | Status | Complexity | Dependencies | Remarks |
| :--- | :--- | :--- | :--- | :--- |
| `<exception>` | To Be Implemented | Medium | `<cstddef>` | `std::exception`, `std::terminate` – base for `bad_alloc`. |
| `<stdexcept>` | Deferred | Low | `<exception>` | Standard exception classes (`runtime_error`, `logic_error`). |
| `<typeinfo>` | **Done** | Low | None | RTTI types (`type_info`). |
| `<csetjmp>` | Deferred | Low | None | `setjmp`/`longjmp` – C compatibility. |
| `<csignal>` | Deferred | Low | None | Signal handling – not commonly used in kernel. |
| `<cstring>` | To Be Implemented | Low | None | `memcpy`, `strlen` etc. – you already have `__memcpy` to wrap. |
| `<cwchar>` | Deferred | Low | None | Wide‑character support – rarely needed in kernel. |
| `<codecvt>` | Deferred | Very High | `<locale>` | Character conversion – deprecated. |
| `<regex>` | Deferred | Very High | `<algorithm>` | Regular expressions – extremely complex. |

