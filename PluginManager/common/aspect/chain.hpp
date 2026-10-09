#pragma once
#include <functional>
#include <utility>
#include <concepts>


namespace aspect {


////////////////////////////////////
/// 示例
// template <typename Mutex, typename LockT>
// class LockAspect {
//     Mutex& mtx_;
// public:
//     explicit LockAspect(Mutex& mtx) : mtx_(mtx) {}

//     template <typename F>
//     auto operator()(F&& f) const {
//         return [&mtx = mtx_, f = std::forward<F>(f)](auto&&... args)
//             -> decltype(auto) {
//             LockT lk(mtx);                        // RAII = 前置/后置通知
//             return std::invoke(f, std::forward<decltype(args)>(args)...);
//         };
//     }
// };

// #include <mutex>
// template <typename Mutex>
// auto lock_aspect(Mutex& mtx) {
//     return LockAspect<Mutex, std::unique_lock<Mutex>>(mtx);
// }
// template <typename Mutex>
// auto shared_lock_aspect(Mutex& mtx) {
//     return LockAspect<Mutex, std::shared_lock<Mutex>>(mtx);
// }
// template<typename Mutex>
// using lock_aspect = LockAspect<Mutex, std::unique_lock<Mutex>>;

// template<typename Mutex>
// using shared_lock_aspect = LockAspect<Mutex, std::shared_lock<Mutex>>;

////////////////////////////////////



// 1. 辅助 trait：计算 chain(Aspects...)(F) 的返回类型
template <typename F, typename... Aspects>
struct ChainResult;

template <typename F>
struct ChainResult<F> {
    using type = F;
};

template <typename F, typename A0, typename... Rest>
    requires std::invocable<A0, typename ChainResult<F, Rest...>::type>
struct ChainResult<F, A0, Rest...> {
    using type = std::invoke_result_t<A0, typename ChainResult<F, Rest...>::type>;
};

// 2. 完整递归 Concept
template <typename F, typename... Aspects>
concept ValidChain = requires {
    typename ChainResult<F, Aspects...>::type; // SFINAE: 若任一层 invocable 失败，此别名不存在
};

// 3. 应用到 Chain 结构体
template <typename... Aspects>
struct Chain {
    std::tuple<Aspects...> aspects;

    template <typename F>
        requires ValidChain<F, Aspects...>
    constexpr auto operator()(F&& f) const & {
        return apply(std::forward<F>(f), std::index_sequence_for<Aspects...>{});
    }

private:
    // ✅ 递归实现：每层返回类型独立推导，无需赋值
    template <typename F>
    static constexpr decltype(auto) wrap_one(F&& f) {
        return std::forward<F>(f);
    }

    template <typename F, typename A0, typename... Rest>
    static constexpr auto wrap_one(F&& f, A0&& a0, Rest&&... rest) {
        // 先递归包裹内层，再用当前切面包裹结果
        auto inner = wrap_one(std::forward<F>(f), std::forward<Rest>(rest)...);
        return std::forward<A0>(a0)(std::move(inner));
    }

    template <typename F, std::size_t... Is>
    constexpr auto apply(F&& f, std::index_sequence<Is...>) const & {
        // 从 tuple 末尾开始展开，保证 chain(A,B,C)(f) => A(B(C(f)))
        return wrap_one(
            std::forward<F>(f),
            std::get<sizeof...(Is) - 1 - Is>(aspects)...
        );
    }
};

template <typename... Aspects>
constexpr auto chain(Aspects&&... as) {
    return Chain<std::decay_t<Aspects>...>{
        std::make_tuple(std::forward<Aspects>(as)...)
    };
}


} // namespace aspect