
#include <sys/mman.h>
#include <cstring>

namespace utils::memmap {
class MemMapping {
   public:
    static constexpr bool kHugePageSupported =
#if defined(MAP_HUGETLB) || defined(FILE_MAP_LARGE_PAGES)
        true;
#else
        false;
#endif

    static MemMapping AllocateHuge(size_t length) {
        return AllocateAnonymous(length, /*huge*/ true);
    }

    /*
    延迟映射（Lazy Mapping）：内存不会立即分配物理页，而是在首次访问时才映射到物理内存（减少初始化开销）。
    零初始化：分配的内存初始值为 0（由操作系统保证）。
    */
    static MemMapping AllocateLazyZeroed(size_t length) {
        return AllocateAnonymous(length, /*huge*/ false);
    }

    MemMapping(const MemMapping&) = delete;
    MemMapping& operator=(const MemMapping&) = delete;

    MemMapping(MemMapping&& other) noexcept { *this = std::move(other); }

    MemMapping& operator=(MemMapping&& other) noexcept {
        if (&other == this) {
            return *this;
        }
        this->~MemMapping();
        // 直接将源对象other的所有成员变量（addr_、length_、page_file_handle_等）字节拷贝到当前对象this中。这种方式比逐个成员赋值更高效（尤其是成员较多时），且能确保所有成员（包括私有成员和平台特定成员）都被完整转移。
        std::memcpy(static_cast<void*>(this), &other, sizeof(*this));
        // 这是 “placement new” 的用法：在other对象已有的内存地址上，重新构造一个MemMapping对象（调用其默认构造函数）。
        new (&other) MemMapping();
        return *this;
    }

    ~MemMapping() {
        if (addr_ != nullptr) {
            auto status = munmap(addr_, length_);
            if (status != 0) {
                // TODO:handle error?
            }
        }
    }

    inline void* Get() const { return addr_; }

    inline size_t Length() const { return length_; }

   private:
    /*
   私有默认构造函数：禁止外部直接创建 MemMapping 对象，必须通过静态成员函数（如 AllocateHuge）创建，确保内存分配逻辑被正确执行。
   */
    MemMapping() {}

    //内存映射的起始地址，默认为 nullptr（表示未分配或分配失败）。
    void* addr_ = nullptr;
    size_t length_ = 0;

    // 内存映射的有效长度（字节）
    static MemMapping AllocateAnonymous(size_t length, bool huge) {
        MemMapping mm;
        mm.length_ = length;
        if (length == 0) {
            // OK to leave addr as nullptr
            return mm;
        }
        int huge_flag = 0;
        if (huge) {
#ifdef MAP_HUGETLB
            huge_flag = MAP_HUGETLB;
#endif
        }
        mm.addr_ = mmap(nullptr, length, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | huge_flag, -1, 0);
        if (mm.addr_ == MAP_FAILED) {
            mm.addr_ = nullptr;
        }
        return mm;
    }
};

/*
void *addr
作用：指定映射区域的起始地址。
取值：
若为 NULL：内核自动选择合适的地址（推荐使用，减少与现有映射冲突的风险）。
若为非 NULL：内核会尽量按此地址映射，但可能调整（最终映射地址以返回值为准）。
注意：映射地址必须按系统页大小（sysconf(_SC_PAGESIZE)）对齐，否则可能失败。

size_t length
作用：映射区域的长度（字节数）。
注意：
实际映射大小会向上取整为页大小的倍数（例如，页大小为 4KB 时，length=5000 会映射 8KB）。
若超过文件大小，超出部分访问时可能触发 SIGBUS 信号（除非文件可扩展）。

int prot（保护权限）
作用：指定映射区域的内存保护方式（可组合多个权限，用 | 连接）。
常用取值：
PROT_READ：允许读映射区域。
PROT_WRITE：允许写映射区域。
PROT_EXEC：允许执行映射区域（如代码段）。
PROT_NONE：禁止任何访问（用于占位或隔离）。
限制：权限不能超过文件本身的访问权限（例如，只读文件不能映射为 PROT_WRITE）。


int flags（映射类型与属性）
作用：控制映射的类型、共享方式及其他特性（关键参数，决定映射行为）。
必选参数（互斥）：
MAP_SHARED：共享映射。对映射区域的修改会同步到文件，且对其他映射该文件的进程可见。常用于多进程共享数据或修改文件。
MAP_PRIVATE：私有映射。对映射区域的修改是进程私有（写时复制，Copy-on-Write），不会同步到文件。常用于读取文件或临时修改（不影响原文件）。
常用可选参数：
MAP_ANONYMOUS（或 MAP_ANON）：匿名映射，不关联任何文件（fd 需设为 -1，offset 忽略）。内存由内核分配，进程退出后释放。常用于进程内临时内存或多进程共享内存（结合 MAP_SHARED）。
MAP_FIXED：强制使用 addr 作为起始地址（若该地址已被占用，会覆盖原有映射，风险较高，谨慎使用）。
MAP_HUGETLB：请求使用大页（Huge Page）分配内存（需系统支持，可减少 TLB misses，提升性能）。
MAP_LOCKED：锁定映射区域到物理内存，避免被交换到 swap（需 CAP_IPC_LOCK 权限，适合低延迟场景）。


int fd
作用：待映射的文件描述符（通过 open 等函数获得）。
特殊情况：
若使用 MAP_ANONYMOUS，fd 需设为 -1。
映射成功后，即使关闭 fd，映射仍有效（文件引用计数由内核维护）。

off_t offset
作用：文件中映射的起始偏移量（从文件开头计算）。
注意：必须是页大小的整数倍（否则 mmap 失败，返回 MAP_FAILED）。


现代操作系统（如 Linux）对用户态内存分配通常采用 “惰性分配” 策略：mmap 仅分配虚拟地址空间，
物理内存（或大页）直到首次访问（读 / 写）时才实际分配（触发缺页中断）。这是为了避免浪费未使用的物理内存。
“惰性分配”：首次访问页时，页进行惰性分配

匿名映射的内存一定会初始化为 0。
文件映射（无 MAP_ANONYMOUS）：内存内容由映射的文件内容决定，不会自动初始化为 0。
*/

template <typename T>
class TypedMemMapping : public MemMapping {
   public:
    TypedMemMapping(MemMapping&& v) noexcept : MemMapping(std::move(v)) {}

    TypedMemMapping& operator=(MemMapping&& v) noexcept {
        MemMapping& base = *this;
        base = std::move(v);
        return *this;
    }

    inline T* Get() const { return static_cast<T*>(MemMapping::Get()); }

    inline size_t Count() const { return MemMapping::Length() / sizeof(T); }

    inline T& operator[](size_t index) const { return Get()[index]; }
};

}  // namespace utils::memmap
