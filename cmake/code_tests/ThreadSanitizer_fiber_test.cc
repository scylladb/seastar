extern "C" {
    void* __tsan_get_current_fiber();
    void* __tsan_create_fiber(unsigned flags);
    void __tsan_destroy_fiber(void* fiber);
    void __tsan_switch_to_fiber(void* fiber, unsigned flags);
}

int main() {
    void* new_fiber = __tsan_create_fiber(0);
    void* main_fiber = __tsan_get_current_fiber();
    __tsan_switch_to_fiber(new_fiber, 0);
    __tsan_switch_to_fiber(main_fiber, 0);
    __tsan_destroy_fiber(new_fiber);
}
