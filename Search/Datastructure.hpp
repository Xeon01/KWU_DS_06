#pragma once

// STL 컨테이너/알고리즘 헤더 없이 기본 C++ 문법만으로 구현한 자료구조 모음
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>

namespace ds {

// ───────────────────────── 기본 유틸리티 ─────────────────────────
template <class T>
struct RemoveRef {
    using type = T;
};

template <class T>
struct RemoveRef<T&> {
    using type = T;
};

template <class T>
struct RemoveRef<T&&> {
    using type = T;
};

template <class T>
constexpr typename RemoveRef<T>::type&& move(T&& x) noexcept {
    return static_cast<typename RemoveRef<T>::type&&>(x);
}

template <class T>
void swap(T& a, T& b) {
    T tmp = ds::move(a);
    a = ds::move(b);
    b = ds::move(tmp);
}

template <class T>
const T& min(const T& a, const T& b) {
    return (b < a) ? b : a;
}

template <class T>
const T& max(const T& a, const T& b) {
    return (a < b) ? b : a;
}

template <class T>
struct Less {
    bool operator()(const T& a, const T& b) const { return a < b; }
};

template <class T>
struct Greater {
    bool operator()(const T& a, const T& b) const { return b < a; }
};

template <class A, class B>
struct Pair {
    A first{};
    B second{};

    Pair() = default;
    Pair(const A& a, const B& b) : first(a), second(b) {}
    Pair(A&& a, B&& b) : first(ds::move(a)), second(ds::move(b)) {}
    Pair(const A& a, B&& b) : first(a), second(ds::move(b)) {}
    Pair(A&& a, const B& b) : first(ds::move(a)), second(b) {}

    bool operator==(const Pair& o) const {
        return first == o.first && second == o.second;
    }
    bool operator<(const Pair& o) const {
        if (first < o.first) return true;
        if (o.first < first) return false;
        return second < o.second;
    }
    bool operator>(const Pair& o) const {
        return o < *this;
    }
};

// ───────────────────────── 고정 크기 배열 (Array) ─────────────────────────
template <class T, size_t N>
struct Array {
    T elems[N > 0 ? N : 1]{};

    constexpr size_t size() const noexcept { return N; }
    constexpr bool empty() const noexcept { return N == 0; }

    T& operator[](size_t i) { return elems[i]; }
    const T& operator[](size_t i) const { return elems[i]; }

    T* data() noexcept { return elems; }
    const T* data() const noexcept { return elems; }

    T* begin() noexcept { return elems; }
    const T* begin() const noexcept { return elems; }
    T* end() noexcept { return elems + N; }
    const T* end() const noexcept { return elems + N; }

    void fill(const T& val) {
        for (size_t i = 0; i < N; ++i) elems[i] = val;
    }
};

// ───────────────────────── 동적 배열 (Vector) ─────────────────────────
template <class T>
class Vector {
public:
    Vector() = default;

    explicit Vector(size_t count) {
        resize(count);
    }

    Vector(size_t count, const T& val) {
        assign(count, val);
    }

    Vector(const Vector& other) {
        reserve(other.size_);
        for (size_t i = 0; i < other.size_; ++i) {
            new (data_ + i) T(other.data_[i]);
        }
        size_ = other.size_;
    }

    Vector(Vector&& other) noexcept
        : data_(other.data_), size_(other.size_), cap_(other.cap_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.cap_ = 0;
    }

    ~Vector() {
        clear();
        ::operator delete(data_);
        data_ = nullptr;
        cap_ = 0;
    }

    Vector& operator=(const Vector& other) {
        if (this == &other) return *this;
        Vector tmp(other);
        swap(tmp);
        return *this;
    }

    Vector& operator=(Vector&& other) noexcept {
        if (this == &other) return *this;
        clear();
        ::operator delete(data_);
        data_ = other.data_;
        size_ = other.size_;
        cap_ = other.cap_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.cap_ = 0;
        return *this;
    }

    void swap(Vector& other) noexcept {
        ds::swap(data_, other.data_);
        ds::swap(size_, other.size_);
        ds::swap(cap_, other.cap_);
    }

    size_t size() const noexcept { return size_; }
    size_t capacity() const noexcept { return cap_; }
    bool empty() const noexcept { return size_ == 0; }

    T* data() noexcept { return data_; }
    const T* data() const noexcept { return data_; }

    T* begin() noexcept { return data_; }
    const T* begin() const noexcept { return data_; }
    T* end() noexcept { return data_ + size_; }
    const T* end() const noexcept { return data_ + size_; }

    T& operator[](size_t i) { return data_[i]; }
    const T& operator[](size_t i) const { return data_[i]; }

    T& at(size_t i) {
        if (i >= size_) throw std::out_of_range("ds::Vector::at out of range");
        return data_[i];
    }
    const T& at(size_t i) const {
        if (i >= size_) throw std::out_of_range("ds::Vector::at out of range");
        return data_[i];
    }

    T& front() { return data_[0]; }
    const T& front() const { return data_[0]; }
    T& back() { return data_[size_ - 1]; }
    const T& back() const { return data_[size_ - 1]; }

    void reserve(size_t newCap) {
        if (newCap <= cap_) return;
        T* next = static_cast<T*>(::operator new(newCap * sizeof(T)));
        size_t moved = 0;
        try {
            for (; moved < size_; ++moved) {
                new (next + moved) T(ds::move(data_[moved]));
            }
        } catch (...) {
            for (size_t j = 0; j < moved; ++j) next[j].~T();
            ::operator delete(next);
            throw;
        }
        for (size_t i = 0; i < size_; ++i) data_[i].~T();
        ::operator delete(data_);
        data_ = next;
        cap_ = newCap;
    }

    void clear() noexcept {
        for (size_t i = 0; i < size_; ++i) data_[i].~T();
        size_ = 0;
    }

    void resize(size_t newSize) {
        if (newSize < size_) {
            for (size_t i = newSize; i < size_; ++i) data_[i].~T();
            size_ = newSize;
            return;
        }
        if (newSize > cap_) reserve(newSize);
        for (size_t i = size_; i < newSize; ++i) {
            new (data_ + i) T();
        }
        size_ = newSize;
    }

    void resize(size_t newSize, const T& val) {
        if (newSize < size_) {
            for (size_t i = newSize; i < size_; ++i) data_[i].~T();
            size_ = newSize;
            return;
        }
        if (newSize > cap_) reserve(newSize);
        for (size_t i = size_; i < newSize; ++i) {
            new (data_ + i) T(val);
        }
        size_ = newSize;
    }

    void assign(size_t count, const T& val) {
        clear();
        resize(count, val);
    }

    void push_back(const T& val) {
        if (size_ == cap_) reserve(cap_ == 0 ? 8 : cap_ * 2);
        new (data_ + size_) T(val);
        ++size_;
    }

    void push_back(T&& val) {
        if (size_ == cap_) reserve(cap_ == 0 ? 8 : cap_ * 2);
        new (data_ + size_) T(ds::move(val));
        ++size_;
    }

    template <class... Args>
    T& emplace_back(Args&&... args) {
        if (size_ == cap_) reserve(cap_ == 0 ? 8 : cap_ * 2);
        T* ptr = new (data_ + size_) T(static_cast<Args&&>(args)...);
        ++size_;
        return *ptr;
    }

    void pop_back() {
        if (size_ > 0) {
            --size_;
            data_[size_].~T();
        }
    }

    bool operator==(const Vector& other) const {
        if (size_ != other.size_) return false;
        for (size_t i = 0; i < size_; ++i) {
            if (!(data_[i] == other.data_[i])) return false;
        }
        return true;
    }

private:
    T* data_ = nullptr;
    size_t size_ = 0;
    size_t cap_ = 0;
};

// ───────────────────────── 알고리즘 (정렬 / 탐색 / 뒤집기) ─────────────────────────
template <class Iter>
void reverse(Iter first, Iter last) {
    while (first < last) {
        --last;
        if (first < last) {
            ds::swap(*first, *last);
            ++first;
        }
    }
}

template <class Iter, class T>
Iter find(Iter first, Iter last, const T& value) {
    for (; first != last; ++first) {
        if (*first == value) return first;
    }
    return last;
}

namespace detail {
template <class T, class Compare>
void mergeSortRange(T* arr, size_t left, size_t right, T* buf, Compare& comp) {
    if (right - left <= 1) return;
    size_t mid = left + (right - left) / 2;
    mergeSortRange(arr, left, mid, buf, comp);
    mergeSortRange(arr, mid, right, buf, comp);

    size_t i = left, j = mid, k = 0;
    while (i < mid && j < right) {
        if (!comp(arr[j], arr[i])) {
            buf[k++] = ds::move(arr[i++]);
        } else {
            buf[k++] = ds::move(arr[j++]);
        }
    }
    while (i < mid) buf[k++] = ds::move(arr[i++]);
    while (j < right) buf[k++] = ds::move(arr[j++]);
    for (size_t t = 0; t < k; ++t) arr[left + t] = ds::move(buf[t]);
}
}  // namespace detail

template <class T, class Compare>
void stableSort(T* first, T* last, Compare comp) {
    if (last <= first) return;
    size_t n = static_cast<size_t>(last - first);
    if (n <= 1) return;
    Vector<T> buf(n);
    detail::mergeSortRange(first, 0, n, buf.data(), comp);
}

template <class T>
void stableSort(T* first, T* last) {
    stableSort(first, last, Less<T>{});
}

template <class T, class Compare>
void sort(T* first, T* last, Compare comp) {
    stableSort(first, last, comp);
}

template <class T>
void sort(T* first, T* last) {
    stableSort(first, last, Less<T>{});
}

// ───────────────────────── 원형 큐 (Queue) ─────────────────────────
template <class T>
class Queue {
public:
    Queue() = default;

    Queue(const Queue& other) {
        reserve(other.count_);
        for (size_t i = 0; i < other.count_; ++i) {
            size_t idx = (other.head_ + i) % other.cap_;
            new (data_ + i) T(other.data_[idx]);
        }
        count_ = other.count_;
        tail_ = count_ % (cap_ ? cap_ : 1);
    }

    Queue(Queue&& other) noexcept
        : data_(other.data_), cap_(other.cap_), head_(other.head_), tail_(other.tail_), count_(other.count_) {
        other.data_ = nullptr;
        other.cap_ = 0;
        other.head_ = 0;
        other.tail_ = 0;
        other.count_ = 0;
    }

    ~Queue() {
        clear();
        ::operator delete(data_);
        data_ = nullptr;
        cap_ = 0;
    }

    Queue& operator=(const Queue& other) {
        if (this == &other) return *this;
        Queue tmp(other);
        swap(tmp);
        return *this;
    }

    Queue& operator=(Queue&& other) noexcept {
        if (this == &other) return *this;
        clear();
        ::operator delete(data_);
        data_ = other.data_;
        cap_ = other.cap_;
        head_ = other.head_;
        tail_ = other.tail_;
        count_ = other.count_;
        other.data_ = nullptr;
        other.cap_ = 0;
        other.head_ = 0;
        other.tail_ = 0;
        other.count_ = 0;
        return *this;
    }

    void swap(Queue& other) noexcept {
        ds::swap(data_, other.data_);
        ds::swap(cap_, other.cap_);
        ds::swap(head_, other.head_);
        ds::swap(tail_, other.tail_);
        ds::swap(count_, other.count_);
    }

    bool empty() const noexcept { return count_ == 0; }
    size_t size() const noexcept { return count_; }

    T& front() { return data_[head_]; }
    const T& front() const { return data_[head_]; }

    void clear() noexcept {
        for (size_t i = 0; i < count_; ++i) {
            data_[(head_ + i) % cap_].~T();
        }
        head_ = 0;
        tail_ = 0;
        count_ = 0;
    }

    void push(const T& val) {
        if (count_ == cap_) reserve(cap_ == 0 ? 16 : cap_ * 2);
        new (data_ + tail_) T(val);
        tail_ = (tail_ + 1) % cap_;
        ++count_;
    }

    void push(T&& val) {
        if (count_ == cap_) reserve(cap_ == 0 ? 16 : cap_ * 2);
        new (data_ + tail_) T(ds::move(val));
        tail_ = (tail_ + 1) % cap_;
        ++count_;
    }

    void pop() {
        if (count_ == 0) return;
        data_[head_].~T();
        head_ = (head_ + 1) % cap_;
        --count_;
    }

private:
    void reserve(size_t newCap) {
        if (newCap <= cap_) return;
        T* next = static_cast<T*>(::operator new(newCap * sizeof(T)));
        size_t moved = 0;
        try {
            for (; moved < count_; ++moved) {
                size_t idx = (head_ + moved) % cap_;
                new (next + moved) T(ds::move(data_[idx]));
            }
        } catch (...) {
            for (size_t j = 0; j < moved; ++j) next[j].~T();
            ::operator delete(next);
            throw;
        }
        for (size_t i = 0; i < count_; ++i) {
            data_[(head_ + i) % cap_].~T();
        }
        ::operator delete(data_);
        data_ = next;
        cap_ = newCap;
        head_ = 0;
        tail_ = count_ % cap_;
    }

    T* data_ = nullptr;
    size_t cap_ = 0;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t count_ = 0;
};

// ───────────────────────── 이진 힙 (PriorityQueue) ─────────────────────────
// Compare(a, b)가 true이면 a가 b보다 우선순위가 낮음 (Less -> 최대 힙, Greater -> 최소 힙)
template <class T, class Compare = Less<T>>
class PriorityQueue {
public:
    PriorityQueue() = default;
    explicit PriorityQueue(const Compare& comp) : comp_(comp) {}

    bool empty() const noexcept { return heap_.empty(); }
    size_t size() const noexcept { return heap_.size(); }
    const T& top() const { return heap_[0]; }

    void clear() noexcept { heap_.clear(); }

    void push(const T& val) {
        heap_.push_back(val);
        siftUp(heap_.size() - 1);
    }

    void push(T&& val) {
        heap_.push_back(ds::move(val));
        siftUp(heap_.size() - 1);
    }

    template <class... Args>
    void emplace(Args&&... args) {
        heap_.emplace_back(static_cast<Args&&>(args)...);
        siftUp(heap_.size() - 1);
    }

    void pop() {
        if (heap_.empty()) return;
        if (heap_.size() > 1) {
            heap_[0] = ds::move(heap_.back());
        }
        heap_.pop_back();
        if (!heap_.empty()) siftDown(0);
    }

private:
    void siftUp(size_t idx) {
        while (idx > 0) {
            size_t parent = (idx - 1) / 2;
            if (comp_(heap_[parent], heap_[idx])) {
                ds::swap(heap_[parent], heap_[idx]);
                idx = parent;
            } else {
                break;
            }
        }
    }

    void siftDown(size_t idx) {
        size_t n = heap_.size();
        while (true) {
            size_t left = 2 * idx + 1;
            size_t right = left + 1;
            size_t best = idx;
            if (left < n && comp_(heap_[best], heap_[left])) best = left;
            if (right < n && comp_(heap_[best], heap_[right])) best = right;
            if (best == idx) break;
            ds::swap(heap_[idx], heap_[best]);
            idx = best;
        }
    }

    Vector<T> heap_;
    Compare comp_{};
};

// ───────────────────────── 해시 함수 및 해시 맵 / 해시 셋 ─────────────────────────
namespace detail {
template <class T>
struct IsByteBuffer {
    template <class U>
    static auto test(int) -> decltype(static_cast<const void*>(static_cast<const U*>(nullptr)->data()),
                                      static_cast<size_t>(static_cast<const U*>(nullptr)->size()),
                                      char{});
    template <class>
    static int test(...);
    static constexpr bool value = sizeof(test<T>(0)) == sizeof(char);
};

inline size_t mix64(uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return static_cast<size_t>(x ^ (x >> 31));
}

template <class K, bool ByteBuf = IsByteBuffer<K>::value>
struct DefaultHash {
    size_t operator()(const K& key) const noexcept {
        return mix64(static_cast<uint64_t>(key));
    }
};

template <class K>
struct DefaultHash<K, true> {
    size_t operator()(const K& key) const noexcept {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(key.data());
        size_t n = static_cast<size_t>(key.size());
        uint64_t h = 14695981039346656037ULL;
        for (size_t i = 0; i < n; ++i) {
            h ^= static_cast<uint64_t>(p[i]);
            h *= 1099511628211ULL;
        }
        return static_cast<size_t>(h);
    }
};
}  // namespace detail

template <class K>
struct Hash : detail::DefaultHash<K> {};

template <class K, class V, class Hasher = Hash<K>>
class HashMap {
private:
    struct Node {
        K key;
        V value;
        Node* next;
    };

public:
    class Iterator {
    public:
        Iterator(Node** buckets, size_t bucketCount, size_t bucketIdx, Node* node)
            : buckets_(buckets), bucketCount_(bucketCount), bucketIdx_(bucketIdx), node_(node) {}

        Pair<const K&, V&> operator*() const {
            return Pair<const K&, V&>(node_->key, node_->value);
        }

        // 호환성: it->first / it->second 대신 it.key() / it.value() 또는 (*it).first 사용,
        // 또한 기존 포인터 문법(it->second)을 위한 프록시 제공
        struct ArrowProxy {
            const K& first;
            V& second;
            const ArrowProxy* operator->() const noexcept { return this; }
        };

        ArrowProxy operator->() const {
            return ArrowProxy{node_->key, node_->value};
        }

        Iterator& operator++() {
            if (node_) node_ = node_->next;
            while (!node_ && ++bucketIdx_ < bucketCount_) {
                node_ = buckets_[bucketIdx_];
            }
            return *this;
        }

        bool operator==(const Iterator& o) const noexcept { return node_ == o.node_; }
        bool operator!=(const Iterator& o) const noexcept { return node_ != o.node_; }

    private:
        Node** buckets_ = nullptr;
        size_t bucketCount_ = 0;
        size_t bucketIdx_ = 0;
        Node* node_ = nullptr;
    };

    class ConstIterator {
    public:
        ConstIterator(Node* const* buckets, size_t bucketCount, size_t bucketIdx, const Node* node)
            : buckets_(buckets), bucketCount_(bucketCount), bucketIdx_(bucketIdx), node_(node) {}

        struct ArrowProxy {
            const K& first;
            const V& second;
            const ArrowProxy* operator->() const noexcept { return this; }
        };

        ArrowProxy operator->() const {
            return ArrowProxy{node_->key, node_->value};
        }

        ConstIterator& operator++() {
            if (node_) node_ = node_->next;
            while (!node_ && ++bucketIdx_ < bucketCount_) {
                node_ = buckets_[bucketIdx_];
            }
            return *this;
        }

        bool operator==(const ConstIterator& o) const noexcept { return node_ == o.node_; }
        bool operator!=(const ConstIterator& o) const noexcept { return node_ != o.node_; }

    private:
        Node* const* buckets_ = nullptr;
        size_t bucketCount_ = 0;
        size_t bucketIdx_ = 0;
        const Node* node_ = nullptr;
    };

    HashMap() = default;

    HashMap(const HashMap& other) {
        if (other.size_ > 0) {
            rehash(other.bucketCount_);
            for (size_t b = 0; b < other.bucketCount_; ++b) {
                for (Node* cur = other.buckets_[b]; cur; cur = cur->next) {
                    insertOrAssign(cur->key, cur->value);
                }
            }
        }
    }

    HashMap(HashMap&& other) noexcept
        : buckets_(other.buckets_), bucketCount_(other.bucketCount_), size_(other.size_) {
        other.buckets_ = nullptr;
        other.bucketCount_ = 0;
        other.size_ = 0;
    }

    ~HashMap() {
        clear();
        delete[] buckets_;
        buckets_ = nullptr;
        bucketCount_ = 0;
    }

    HashMap& operator=(const HashMap& other) {
        if (this == &other) return *this;
        HashMap tmp(other);
        swap(tmp);
        return *this;
    }

    HashMap& operator=(HashMap&& other) noexcept {
        if (this == &other) return *this;
        clear();
        delete[] buckets_;
        buckets_ = other.buckets_;
        bucketCount_ = other.bucketCount_;
        size_ = other.size_;
        other.buckets_ = nullptr;
        other.bucketCount_ = 0;
        other.size_ = 0;
        return *this;
    }

    void swap(HashMap& other) noexcept {
        ds::swap(buckets_, other.buckets_);
        ds::swap(bucketCount_, other.bucketCount_);
        ds::swap(size_, other.size_);
    }

    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    void clear() noexcept {
        for (size_t b = 0; b < bucketCount_; ++b) {
            Node* cur = buckets_[b];
            while (cur) {
                Node* nxt = cur->next;
                delete cur;
                cur = nxt;
            }
            buckets_[b] = nullptr;
        }
        size_ = 0;
    }

    void reserve(size_t expectedElements) {
        size_t needed = (expectedElements * 4) / 3 + 1;
        if (needed > bucketCount_) rehash(nextPow2(needed));
    }

    Iterator find(const K& key) {
        if (bucketCount_ == 0) return end();
        size_t b = hasher_(key) & (bucketCount_ - 1);
        for (Node* cur = buckets_[b]; cur; cur = cur->next) {
            if (cur->key == key) return Iterator(buckets_, bucketCount_, b, cur);
        }
        return end();
    }

    ConstIterator find(const K& key) const {
        if (bucketCount_ == 0) return end();
        size_t b = hasher_(key) & (bucketCount_ - 1);
        for (const Node* cur = buckets_[b]; cur; cur = cur->next) {
            if (cur->key == key) return ConstIterator(buckets_, bucketCount_, b, cur);
        }
        return end();
    }

    size_t count(const K& key) const {
        return find(key) != end() ? 1 : 0;
    }

    V& operator[](const K& key) {
        if (bucketCount_ == 0 || (size_ + 1) * 4 > bucketCount_ * 3) {
            rehash(bucketCount_ == 0 ? 16 : bucketCount_ * 2);
        }
        size_t b = hasher_(key) & (bucketCount_ - 1);
        for (Node* cur = buckets_[b]; cur; cur = cur->next) {
            if (cur->key == key) return cur->value;
        }
        Node* node = new Node{key, V{}, buckets_[b]};
        buckets_[b] = node;
        ++size_;
        return node->value;
    }

    void insertOrAssign(const K& key, const V& value) {
        (*this)[key] = value;
    }

    Iterator begin() {
        for (size_t b = 0; b < bucketCount_; ++b) {
            if (buckets_[b]) return Iterator(buckets_, bucketCount_, b, buckets_[b]);
        }
        return end();
    }

    Iterator end() {
        return Iterator(buckets_, bucketCount_, bucketCount_, nullptr);
    }

    ConstIterator begin() const {
        for (size_t b = 0; b < bucketCount_; ++b) {
            if (buckets_[b]) return ConstIterator(buckets_, bucketCount_, b, buckets_[b]);
        }
        return end();
    }

    ConstIterator end() const {
        return ConstIterator(buckets_, bucketCount_, bucketCount_, nullptr);
    }

private:
    static size_t nextPow2(size_t v) noexcept {
        size_t p = 16;
        while (p < v) p *= 2;
        return p;
    }

    void rehash(size_t newBucketCount) {
        Node** next = new Node*[newBucketCount]();
        for (size_t b = 0; b < bucketCount_; ++b) {
            Node* cur = buckets_[b];
            while (cur) {
                Node* nxt = cur->next;
                size_t nb = hasher_(cur->key) & (newBucketCount - 1);
                cur->next = next[nb];
                next[nb] = cur;
                cur = nxt;
            }
        }
        delete[] buckets_;
        buckets_ = next;
        bucketCount_ = newBucketCount;
    }

    Node** buckets_ = nullptr;
    size_t bucketCount_ = 0;
    size_t size_ = 0;
    Hasher hasher_{};
};

template <class K, class Hasher = Hash<K>>
class HashSet {
public:
    HashSet() = default;

    size_t size() const noexcept { return map_.size(); }
    bool empty() const noexcept { return map_.empty(); }
    void clear() noexcept { map_.clear(); }
    void reserve(size_t n) { map_.reserve(n); }
    bool contains(const K& key) const { return map_.count(key) > 0; }
    size_t count(const K& key) const { return map_.count(key); }

    // 새로 삽입되면 true, 이미 존재하면 false 반환
    bool insert(const K& key) {
        if (map_.count(key) > 0) return false;
        map_[key] = 1;
        return true;
    }

private:
    HashMap<K, char, Hasher> map_;
};

// ───────────────────────── 정렬 집합: AVL 트리 (OrderedSet) ─────────────────────────
template <class T>
class OrderedSet {
private:
    struct Node {
        T key;
        Node* left = nullptr;
        Node* right = nullptr;
        int height = 1;
    };

public:
    OrderedSet() = default;

    OrderedSet(const OrderedSet& other) {
        root_ = cloneTree(other.root_);
        size_ = other.size_;
    }

    OrderedSet(OrderedSet&& other) noexcept
        : root_(other.root_), size_(other.size_) {
        other.root_ = nullptr;
        other.size_ = 0;
    }

    ~OrderedSet() {
        clear();
    }

    OrderedSet& operator=(const OrderedSet& other) {
        if (this == &other) return *this;
        OrderedSet tmp(other);
        swap(tmp);
        return *this;
    }

    OrderedSet& operator=(OrderedSet&& other) noexcept {
        if (this == &other) return *this;
        clear();
        root_ = other.root_;
        size_ = other.size_;
        other.root_ = nullptr;
        other.size_ = 0;
        return *this;
    }

    void swap(OrderedSet& other) noexcept {
        ds::swap(root_, other.root_);
        ds::swap(size_, other.size_);
    }

    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    void clear() noexcept {
        destroyTree(root_);
        root_ = nullptr;
        size_ = 0;
    }

    bool insert(const T& key) {
        bool inserted = false;
        root_ = insertNode(root_, key, inserted);
        if (inserted) ++size_;
        return inserted;
    }

    bool contains(const T& key) const {
        const Node* cur = root_;
        while (cur) {
            if (key < cur->key) cur = cur->left;
            else if (cur->key < key) cur = cur->right;
            else return true;
        }
        return false;
    }

    Vector<T> toSortedVector() const {
        Vector<T> out;
        out.reserve(size_);
        inorder(root_, out);
        return out;
    }

private:
    static int heightOf(const Node* n) noexcept { return n ? n->height : 0; }

    static void updateHeight(Node* n) noexcept {
        int lh = heightOf(n->left);
        int rh = heightOf(n->right);
        n->height = 1 + (lh > rh ? lh : rh);
    }

    static int balanceFactor(const Node* n) noexcept {
        return n ? heightOf(n->left) - heightOf(n->right) : 0;
    }

    static Node* rotateRight(Node* y) noexcept {
        Node* x = y->left;
        Node* t2 = x->right;
        x->right = y;
        y->left = t2;
        updateHeight(y);
        updateHeight(x);
        return x;
    }

    static Node* rotateLeft(Node* x) noexcept {
        Node* y = x->right;
        Node* t2 = y->left;
        y->left = x;
        x->right = t2;
        updateHeight(x);
        updateHeight(y);
        return y;
    }

    static Node* insertNode(Node* node, const T& key, bool& inserted) {
        if (!node) {
            inserted = true;
            return new Node{key, nullptr, nullptr, 1};
        }
        if (key < node->key) {
            node->left = insertNode(node->left, key, inserted);
        } else if (node->key < key) {
            node->right = insertNode(node->right, key, inserted);
        } else {
            return node;
        }

        updateHeight(node);
        int bf = balanceFactor(node);
        if (bf > 1 && key < node->left->key) return rotateRight(node);
        if (bf < -1 && node->right->key < key) return rotateLeft(node);
        if (bf > 1 && node->left->key < key) {
            node->left = rotateLeft(node->left);
            return rotateRight(node);
        }
        if (bf < -1 && key < node->right->key) {
            node->right = rotateRight(node->right);
            return rotateLeft(node);
        }
        return node;
    }

    static void inorder(const Node* node, Vector<T>& out) {
        if (!node) return;
        inorder(node->left, out);
        out.push_back(node->key);
        inorder(node->right, out);
    }

    static Node* cloneTree(const Node* node) {
        if (!node) return nullptr;
        Node* copy = new Node{node->key, nullptr, nullptr, node->height};
        try {
            copy->left = cloneTree(node->left);
            copy->right = cloneTree(node->right);
        } catch (...) {
            destroyTree(copy);
            throw;
        }
        return copy;
    }

    static void destroyTree(Node* node) noexcept {
        if (!node) return;
        destroyTree(node->left);
        destroyTree(node->right);
        delete node;
    }

    Node* root_ = nullptr;
    size_t size_ = 0;
};

}  // namespace ds
