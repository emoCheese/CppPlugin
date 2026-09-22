# TODO

## 插件扫描，加载去重

## 线程安全化

load unload 加锁
同时防止 递归加锁或者 使用递归锁


## 添加引用计数，防止悬垂引用

使用 std::shared_ptr 包装 IPlugin* ptr，并自定义 deleter