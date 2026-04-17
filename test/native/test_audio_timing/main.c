#include <unity.h>
#include <stdint.h>
#include <stdbool.h>

// Mock FreeRTOS types
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) (ms)

// Mock timing state
static TickType_t s_current_tick = 1000;
static int s_delay_calls = 0;
static int s_delay_until_calls = 0;

TickType_t xTaskGetTickCount(void) {
    return s_current_tick;
}

void vTaskDelay(const TickType_t xTicksToDelay) {
    s_delay_calls++;
    s_current_tick += xTicksToDelay;
}

void vTaskDelayUntil(TickType_t *const pxPreviousWakeTime, const TickType_t xTimeIncrement) {
    s_delay_until_calls++;
    TickType_t target = *pxPreviousWakeTime + xTimeIncrement;
    if (target > s_current_tick) {
        s_current_tick = target;
    }
    *pxPreviousWakeTime = s_current_tick;
}

// Mock pipeline state
typedef enum {
    ADF_INPUT_MODE_AUX = 0,
    ADF_INPUT_MODE_TONE,
    ADF_INPUT_MODE_USB
} adf_input_mode_t;

typedef struct {
    bool running;
    adf_input_mode_t input_mode;
} mock_pipeline_t;

// The logic we want to test (stripped from tx_capture_task)
int run_capture_logic_iter(mock_pipeline_t *pipeline, adf_input_mode_t *last_mode, TickType_t *last_wake_time) {
    adf_input_mode_t mode = pipeline->input_mode;
    const TickType_t frame_ticks = 20;

    if (mode != *last_mode) {
        *last_wake_time = xTaskGetTickCount();
        *last_mode = mode;
    }

    if (mode == ADF_INPUT_MODE_TONE || mode == ADF_INPUT_MODE_USB || mode == ADF_INPUT_MODE_AUX) {
        vTaskDelayUntil(last_wake_time, frame_ticks);
        return 0;
    }
    return 0;
}

void test_capture_no_spin_on_mode_switch(void) {
    mock_pipeline_t pipeline = { .running = true, .input_mode = ADF_INPUT_MODE_AUX };
    adf_input_mode_t last_mode = ADF_INPUT_MODE_AUX;
    TickType_t last_wake_time = xTaskGetTickCount();
    
    s_current_tick = 1000;
    s_delay_until_calls = 0;

    // 1. Run in AUX mode
    for(int i=0; i<10; i++) {
        run_capture_logic_iter(&pipeline, &last_mode, &last_wake_time);
    }
    TEST_ASSERT_EQUAL_UINT32(1000 + (10 * 20), s_current_tick);

    // 2. Switch to TONE mode
    pipeline.input_mode = ADF_INPUT_MODE_TONE;
    run_capture_logic_iter(&pipeline, &last_mode, &last_wake_time);
    
    TEST_ASSERT_EQUAL_INT(11, s_delay_until_calls);
    TEST_ASSERT_EQUAL_UINT32(1200 + 20, s_current_tick);
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_capture_no_spin_on_mode_switch);
    return UNITY_END();
}
