/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_core.h"
#include "claw_core_llm.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "unity.h"
#include "unity_test_runner.h"

#define TEST_RECORD_MAX 64
#define TEST_CAPTURE_MAX 16

typedef enum {
    TEST_BACKEND_FINAL = 0,
    TEST_BACKEND_HTTP_ABORT,
    TEST_BACKEND_AFTER_LLM_INTERRUPT,
    TEST_BACKEND_TOOL_INTERRUPT,
} test_backend_mode_t;

typedef struct {
    char session_id[32];
    claw_session_record_type_t type;
    char *text;
    char *message_json;
} test_record_t;

static test_record_t s_records[TEST_RECORD_MAX];
static size_t s_record_count;
static char *s_captured_messages[TEST_CAPTURE_MAX];
static size_t s_capture_count;
static test_backend_mode_t s_backend_mode;
static uint32_t s_active_request_id;
static char s_active_session_id[32];
static size_t s_backend_call_count;
static size_t s_tool_call_count;
static bool s_core_ready;
static bool s_saw_building_phase;
static bool s_saw_before_build_phase;
static bool s_saw_in_llm_phase;
static bool s_request_start_interrupt_enabled;
static bool s_interrupt_provider_enabled;
static bool s_interrupt_provider_done;
static bool s_fail_user_persist;
static esp_err_t s_queue_full_err = ESP_OK;

static char *test_dup_string(const char *text)
{
    return text ? strdup(text) : NULL;
}

static void test_clear_captures(void)
{
    for (size_t i = 0; i < s_capture_count; i++) {
        free(s_captured_messages[i]);
        s_captured_messages[i] = NULL;
    }
    s_capture_count = 0;
}

static void test_clear_records(void)
{
    for (size_t i = 0; i < s_record_count; i++) {
        free(s_records[i].text);
        free(s_records[i].message_json);
        memset(&s_records[i], 0, sizeof(s_records[i]));
    }
    s_record_count = 0;
}

static void test_reset_scenario(test_backend_mode_t mode, uint32_t request_id)
{
    test_clear_captures();
    s_backend_mode = mode;
    s_active_request_id = request_id;
    s_active_session_id[0] = '\0';
    s_backend_call_count = 0;
    s_tool_call_count = 0;
    s_saw_building_phase = false;
    s_saw_before_build_phase = false;
    s_saw_in_llm_phase = false;
    s_request_start_interrupt_enabled = false;
    s_interrupt_provider_enabled = false;
    s_interrupt_provider_done = false;
    s_fail_user_persist = false;
    s_queue_full_err = ESP_OK;
}

static esp_err_t test_persist_session(const claw_session_persist_batch_t *batch, void *user_ctx)
{
    (void)user_ctx;

    if (!batch || !batch->session_id || !batch->records) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_fail_user_persist) {
        for (size_t i = 0; i < batch->record_count; i++) {
            if (batch->records[i].type == CLAW_SESSION_RECORD_USER) {
                return ESP_FAIL;
            }
        }
    }
    if (s_record_count + batch->record_count > TEST_RECORD_MAX) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < batch->record_count; i++) {
        const claw_session_record_t *src = &batch->records[i];
        test_record_t *dst = &s_records[s_record_count++];

        strlcpy(dst->session_id, batch->session_id, sizeof(dst->session_id));
        dst->type = src->type;
        dst->text = test_dup_string(src->text);
        dst->message_json = test_dup_string(src->message_json);
        if ((src->text && !dst->text) || (src->message_json && !dst->message_json)) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static esp_err_t test_session_history_collect(const claw_core_request_t *request,
                                              claw_core_context_t *out_context,
                                              void *user_ctx)
{
    cJSON *messages = NULL;
    char *json = NULL;

    (void)user_ctx;
    if (!request || !out_context || !request->session_id) {
        return ESP_ERR_INVALID_ARG;
    }

    messages = cJSON_CreateArray();
    if (!messages) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < s_record_count; i++) {
        test_record_t *record = &s_records[i];

        if (strcmp(record->session_id, request->session_id) != 0) {
            continue;
        }
        if (record->type == CLAW_SESSION_RECORD_USER) {
            cJSON *msg = cJSON_CreateObject();

            if (!msg ||
                    !cJSON_AddStringToObject(msg, "role", "user") ||
                    !cJSON_AddStringToObject(msg, "content", record->text ? record->text : "")) {
                cJSON_Delete(msg);
                cJSON_Delete(messages);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddItemToArray(messages, msg);
        } else if (record->type == CLAW_SESSION_RECORD_ASSISTANT_FINAL) {
            cJSON *msg = cJSON_CreateObject();

            if (!msg ||
                    !cJSON_AddStringToObject(msg, "role", "assistant") ||
                    !cJSON_AddStringToObject(msg, "content", record->text ? record->text : "")) {
                cJSON_Delete(msg);
                cJSON_Delete(messages);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddItemToArray(messages, msg);
        } else if (record->type == CLAW_SESSION_RECORD_ASSISTANT_TOOL && record->message_json) {
            cJSON *msg = cJSON_Parse(record->message_json);

            if (!msg) {
                cJSON_Delete(messages);
                return ESP_FAIL;
            }
            cJSON_AddItemToArray(messages, msg);
        } else if (record->type == CLAW_SESSION_RECORD_TOOL_RESULT && record->message_json) {
            cJSON *array = cJSON_Parse(record->message_json);

            if (!array || !cJSON_IsArray(array)) {
                cJSON_Delete(array);
                cJSON_Delete(messages);
                return ESP_FAIL;
            }
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, array) {
                cJSON *dup = cJSON_Duplicate(item, true);

                if (!dup) {
                    cJSON_Delete(array);
                    cJSON_Delete(messages);
                    return ESP_ERR_NO_MEM;
                }
                cJSON_AddItemToArray(messages, dup);
            }
            cJSON_Delete(array);
        }
    }

    if (cJSON_GetArraySize(messages) == 0) {
        cJSON_Delete(messages);
        return ESP_ERR_NOT_FOUND;
    }

    json = cJSON_PrintUnformatted(messages);
    cJSON_Delete(messages);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    out_context->kind = CLAW_CORE_CONTEXT_KIND_MESSAGES;
    out_context->content = json;
    return ESP_OK;
}

static esp_err_t test_request_start(const claw_core_request_t *request, void *user_ctx)
{
    uint32_t interrupt_request_id = 0;

    (void)user_ctx;

    if (!request || !s_request_start_interrupt_enabled ||
            request->request_id != s_active_request_id) {
        return ESP_OK;
    }

    s_saw_before_build_phase =
        claw_core_get_agent_loop_phase() ==
        CLAW_CORE_AGENT_LOOP_PHASE_BEFORE_BUILD_ITERATION_CONTEXT;
    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_core_submit_user_message_interrupt_for_session(request->session_id,
                                                                         "B",
                                                                         &interrupt_request_id));
    TEST_ASSERT_EQUAL(request->request_id, interrupt_request_id);
    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_core_submit_user_message_interrupt_for_session(request->session_id,
                                                                         "C",
                                                                         NULL));
    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_core_submit_user_message_interrupt_for_session(request->session_id,
                                                                         "D",
                                                                         NULL));
    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_core_submit_user_message_interrupt_for_session(request->session_id,
                                                                         "E",
                                                                         NULL));
    s_queue_full_err = claw_core_submit_user_message_interrupt_for_session(request->session_id,
                                                                          "F",
                                                                          NULL);
    return ESP_OK;
}

static esp_err_t test_interrupt_provider_collect(const claw_core_request_t *request,
                                                 claw_core_context_t *out_context,
                                                 void *user_ctx)
{
    (void)out_context;
    (void)user_ctx;

    if (!request || !s_interrupt_provider_enabled || s_interrupt_provider_done) {
        return ESP_ERR_NOT_FOUND;
    }
    if (request->request_id != s_active_request_id) {
        return ESP_ERR_NOT_FOUND;
    }

    s_saw_building_phase =
        claw_core_get_agent_loop_phase() == CLAW_CORE_AGENT_LOOP_PHASE_BUILDING_ITERATION_CONTEXT;
    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_core_submit_user_message_interrupt_for_session(request->session_id,
                                                                         "B",
                                                                         NULL));
    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_core_submit_user_message_interrupt_for_session(request->session_id,
                                                                         "C",
                                                                         NULL));
    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_core_submit_user_message_interrupt_for_session(request->session_id,
                                                                         "D",
                                                                         NULL));
    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_core_submit_user_message_interrupt_for_session(request->session_id,
                                                                         "E",
                                                                         NULL));
    s_queue_full_err = claw_core_submit_user_message_interrupt_for_session(request->session_id,
                                                                          "F",
                                                                          NULL);
    s_interrupt_provider_done = true;
    return ESP_ERR_NOT_FOUND;
}

static const claw_core_context_provider_t s_session_provider = {
    .name = "Test Session History",
    .collect = test_session_history_collect,
    .user_ctx = NULL,
    .flags = CLAW_CORE_CONTEXT_PROVIDER_FLAG_REQUEST_START_ONLY,
};

static const claw_core_context_provider_t s_interrupt_provider = {
    .name = "Test Interrupt Provider",
    .collect = test_interrupt_provider_collect,
    .user_ctx = NULL,
};

static esp_err_t test_call_cap(const char *cap_name,
                               const char *input_json,
                               const claw_core_request_t *request,
                               char **out_output,
                               void *user_ctx)
{
    (void)cap_name;
    (void)input_json;
    (void)user_ctx;

    s_tool_call_count++;
    if (s_backend_mode == TEST_BACKEND_TOOL_INTERRUPT && s_tool_call_count == 1) {
        TEST_ASSERT_EQUAL(CLAW_CORE_AGENT_LOOP_PHASE_RUNNING_TOOL, claw_core_get_agent_loop_phase());
        TEST_ASSERT_EQUAL(ESP_OK,
                          claw_core_submit_user_message_interrupt_for_session(request->session_id,
                                                                             "I",
                                                                             NULL));
    }

    *out_output = test_dup_string("{\"ok\":true}");
    return *out_output ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t test_backend_init(const claw_llm_runtime_config_t *config,
                                   const claw_llm_model_profile_t *profile,
                                   void **out_backend_ctx,
                                   char **out_error_message)
{
    (void)config;
    (void)profile;
    (void)out_error_message;

    *out_backend_ctx = NULL;
    return ESP_OK;
}

static esp_err_t test_set_final_response(claw_llm_response_t *out_response, const char *text)
{
    out_response->text = test_dup_string(text);
    out_response->raw_message_json = test_dup_string("{\"role\":\"assistant\",\"content\":\"final\"}");
    if (!out_response->text || !out_response->raw_message_json) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t test_set_tool_response(claw_llm_response_t *out_response, size_t tool_call_count)
{
    const char *raw_message_json = NULL;

    if (tool_call_count == 1) {
        raw_message_json =
            "{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\","
            "\"function\":{\"name\":\"test_tool\",\"arguments\":\"{}\"}}]}";
    } else if (tool_call_count == 2) {
        raw_message_json =
            "{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\","
            "\"function\":{\"name\":\"test_tool\",\"arguments\":\"{}\"}},"
            "{\"id\":\"call_2\",\"type\":\"function\","
            "\"function\":{\"name\":\"test_tool_2\",\"arguments\":\"{}\"}}]}";
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    out_response->raw_message_json = test_dup_string(raw_message_json);
    out_response->tool_calls = calloc(tool_call_count, sizeof(out_response->tool_calls[0]));
    if (!out_response->raw_message_json || !out_response->tool_calls) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < tool_call_count; i++) {
        char id[16];
        const char *name = i == 0 ? "test_tool" : "test_tool_2";

        snprintf(id, sizeof(id), "call_%u", (unsigned int)(i + 1));
        out_response->tool_calls[i].id = test_dup_string(id);
        out_response->tool_calls[i].name = test_dup_string(name);
        out_response->tool_calls[i].arguments_json = test_dup_string("{}");
        if (!out_response->tool_calls[i].id ||
                !out_response->tool_calls[i].name ||
                !out_response->tool_calls[i].arguments_json) {
            return ESP_ERR_NO_MEM;
        }
    }
    out_response->tool_call_count = tool_call_count;
    return ESP_OK;
}

static esp_err_t test_backend_chat(void *backend_ctx,
                                   const claw_llm_model_profile_t *profile,
                                   const claw_llm_chat_request_t *request,
                                   claw_llm_response_t *out_response,
                                   char **out_error_message)
{
    (void)backend_ctx;
    (void)profile;

    s_saw_in_llm_phase =
        s_saw_in_llm_phase ||
        claw_core_get_agent_loop_phase() == CLAW_CORE_AGENT_LOOP_PHASE_IN_LLM_HTTP;
    if (s_capture_count < TEST_CAPTURE_MAX) {
        s_captured_messages[s_capture_count++] = cJSON_PrintUnformatted(request->messages);
    }
    s_backend_call_count++;

    if (s_backend_mode == TEST_BACKEND_HTTP_ABORT && s_backend_call_count == 1) {
        TEST_ASSERT_EQUAL(ESP_OK,
                          claw_core_submit_user_message_interrupt_for_session(s_active_session_id,
                                                                             "G",
                                                                             NULL));
        *out_error_message = test_dup_string("aborted");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_backend_mode == TEST_BACKEND_AFTER_LLM_INTERRUPT && s_backend_call_count == 1) {
        TEST_ASSERT_EQUAL(ESP_OK,
                          claw_core_submit_user_message_interrupt_for_session(s_active_session_id,
                                                                             "H",
                                                                             NULL));
        return test_set_tool_response(out_response, 2);
    }
    if (s_backend_mode == TEST_BACKEND_TOOL_INTERRUPT && s_backend_call_count == 1) {
        return test_set_tool_response(out_response, 1);
    }

    return test_set_final_response(out_response, "final");
}

static esp_err_t test_backend_infer_media(void *backend_ctx,
                                          const claw_llm_model_profile_t *profile,
                                          const claw_llm_media_request_t *request,
                                          char **out_text,
                                          char **out_error_message)
{
    (void)backend_ctx;
    (void)profile;
    (void)request;
    (void)out_error_message;

    *out_text = test_dup_string("media");
    return *out_text ? ESP_OK : ESP_ERR_NO_MEM;
}

static void test_backend_deinit(void *backend_ctx)
{
    (void)backend_ctx;
}

static const claw_llm_backend_vtable_t s_backend_vtable = {
    .init = test_backend_init,
    .chat = test_backend_chat,
    .infer_media = test_backend_infer_media,
    .deinit = test_backend_deinit,
};

static void ensure_core_ready(void)
{
    if (s_core_ready) {
        return;
    }

    const claw_llm_custom_backend_registration_t registration = {
        .id = "core_interrupt_test",
        .vtable = &s_backend_vtable,
        .defaults = {
            .auth_type = "none",
            .chat_path = "",
            .max_tokens_field = "max_tokens",
        },
    };
    const claw_core_config_t config = {
        .api_key = "test",
        .backend_type = "core_interrupt_test",
        .model = "test",
        .system_prompt = "system",
        .persist_session = test_persist_session,
        .on_request_start = test_request_start,
        .call_cap = test_call_cap,
        .supports_tools = true,
        .request_queue_len = 4,
        .response_queue_len = 4,
        .max_context_providers = 2,
    };

    TEST_ASSERT_EQUAL(ESP_OK, claw_core_llm_register_custom_backend(&registration));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_init(&config));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_add_context_provider(&s_session_provider));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_add_context_provider(&s_interrupt_provider));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_start());
    s_core_ready = true;
}

static void submit_and_expect_ok(uint32_t request_id, const char *session_id, const char *text)
{
    claw_core_request_t request = {
        .request_id = request_id,
        .session_id = session_id,
        .user_text = text,
    };
    claw_core_response_t response = {0};

    strlcpy(s_active_session_id, session_id ? session_id : "", sizeof(s_active_session_id));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_submit(&request, 1000));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_receive_for(request_id, &response, 5000));
    TEST_ASSERT_EQUAL(request_id, response.request_id);
    TEST_ASSERT_EQUAL(CLAW_CORE_RESPONSE_STATUS_OK, response.status);
    TEST_ASSERT_NOT_NULL(response.text);
    claw_core_response_free(&response);
}

static size_t count_session_records(const char *session_id, claw_session_record_type_t type)
{
    size_t count = 0;

    for (size_t i = 0; i < s_record_count; i++) {
        if (strcmp(s_records[i].session_id, session_id) == 0 && s_records[i].type == type) {
            count++;
        }
    }
    return count;
}

static void assert_messages_contain_users(size_t capture_index,
                                          const char *u0,
                                          const char *u1,
                                          const char *u2,
                                          const char *u3,
                                          const char *u4)
{
    const char *expected[] = {u0, u1, u2, u3, u4, NULL};
    cJSON *messages = cJSON_Parse(s_captured_messages[capture_index]);
    size_t user_index = 0;

    TEST_ASSERT_NOT_NULL(messages);
    TEST_ASSERT_TRUE(cJSON_IsArray(messages));
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, messages) {
        cJSON *role = cJSON_GetObjectItem(item, "role");
        cJSON *content = cJSON_GetObjectItem(item, "content");

        if (!cJSON_IsString(role) || strcmp(role->valuestring, "user") != 0) {
            continue;
        }
        TEST_ASSERT_LESS_THAN((sizeof(expected) / sizeof(expected[0])) - 1, user_index);
        TEST_ASSERT_NOT_NULL(expected[user_index]);
        TEST_ASSERT_TRUE(cJSON_IsString(content));
        TEST_ASSERT_EQUAL_STRING(expected[user_index], content->valuestring);
        user_index++;
    }
    TEST_ASSERT_NULL(expected[user_index]);
    cJSON_Delete(messages);
}

void setUp(void)
{
    test_clear_records();
    test_clear_captures();
}

void tearDown(void)
{
    test_clear_records();
    test_clear_captures();
}

TEST_CASE("claw_core handles active user interrupts with FIFO restarts", "[claw_core]")
{
    ensure_core_ready();

    test_reset_scenario(TEST_BACKEND_FINAL, 1001);
    s_request_start_interrupt_enabled = true;
    submit_and_expect_ok(1001, "s-before-build", "A");
    TEST_ASSERT_TRUE(s_saw_before_build_phase);
    TEST_ASSERT_TRUE(s_saw_in_llm_phase);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, s_queue_full_err);
    TEST_ASSERT_EQUAL(1, s_backend_call_count);
    TEST_ASSERT_EQUAL(5, count_session_records("s-before-build", CLAW_SESSION_RECORD_USER));
    TEST_ASSERT_EQUAL(1, count_session_records("s-before-build", CLAW_SESSION_RECORD_ASSISTANT_FINAL));
    assert_messages_contain_users(0, "A", "B", "C", "D", "E");

    test_reset_scenario(TEST_BACKEND_FINAL, 1006);
    s_request_start_interrupt_enabled = true;
    s_fail_user_persist = true;
    submit_and_expect_ok(1006, "s-persist-fail", "A0");
    TEST_ASSERT_TRUE(s_saw_before_build_phase);
    TEST_ASSERT_TRUE(s_saw_in_llm_phase);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, s_queue_full_err);
    TEST_ASSERT_EQUAL(1, s_backend_call_count);
    TEST_ASSERT_EQUAL(0, count_session_records("s-persist-fail", CLAW_SESSION_RECORD_USER));
    TEST_ASSERT_EQUAL(1, count_session_records("s-persist-fail", CLAW_SESSION_RECORD_ASSISTANT_FINAL));
    assert_messages_contain_users(0, "A0", "B", "C", "D", "E");

    test_reset_scenario(TEST_BACKEND_FINAL, 1005);
    s_interrupt_provider_enabled = true;
    submit_and_expect_ok(1005, "s-before-http", "A1");
    TEST_ASSERT_TRUE(s_saw_building_phase);
    TEST_ASSERT_TRUE(s_saw_in_llm_phase);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, s_queue_full_err);
    TEST_ASSERT_EQUAL(1, s_backend_call_count);
    TEST_ASSERT_EQUAL(5, count_session_records("s-before-http", CLAW_SESSION_RECORD_USER));
    TEST_ASSERT_EQUAL(1, count_session_records("s-before-http", CLAW_SESSION_RECORD_ASSISTANT_FINAL));
    assert_messages_contain_users(0, "A1", "B", "C", "D", "E");

    test_reset_scenario(TEST_BACKEND_HTTP_ABORT, 1002);
    submit_and_expect_ok(1002, "s-http", "F");
    TEST_ASSERT_EQUAL(2, s_backend_call_count);
    TEST_ASSERT_EQUAL(2, count_session_records("s-http", CLAW_SESSION_RECORD_USER));
    TEST_ASSERT_EQUAL(1, count_session_records("s-http", CLAW_SESSION_RECORD_ASSISTANT_FINAL));
    assert_messages_contain_users(1, "F", "G", NULL, NULL, NULL);

    test_reset_scenario(TEST_BACKEND_AFTER_LLM_INTERRUPT, 1003);
    submit_and_expect_ok(1003, "s-after-llm", "A2");
    TEST_ASSERT_EQUAL(2, s_backend_call_count);
    TEST_ASSERT_EQUAL(0, s_tool_call_count);
    TEST_ASSERT_EQUAL(2, count_session_records("s-after-llm", CLAW_SESSION_RECORD_USER));
    TEST_ASSERT_EQUAL(0, count_session_records("s-after-llm", CLAW_SESSION_RECORD_ASSISTANT_TOOL));
    TEST_ASSERT_EQUAL(0, count_session_records("s-after-llm", CLAW_SESSION_RECORD_TOOL_RESULT));
    assert_messages_contain_users(1, "A2", "H", NULL, NULL, NULL);

    test_reset_scenario(TEST_BACKEND_TOOL_INTERRUPT, 1004);
    submit_and_expect_ok(1004, "s-tool", "A3");
    TEST_ASSERT_EQUAL(2, s_backend_call_count);
    TEST_ASSERT_EQUAL(1, s_tool_call_count);
    TEST_ASSERT_EQUAL(2, count_session_records("s-tool", CLAW_SESSION_RECORD_USER));
    TEST_ASSERT_EQUAL(1, count_session_records("s-tool", CLAW_SESSION_RECORD_ASSISTANT_TOOL));
    TEST_ASSERT_EQUAL(1, count_session_records("s-tool", CLAW_SESSION_RECORD_TOOL_RESULT));
    assert_messages_contain_users(1, "A3", "I", NULL, NULL, NULL);
}

void app_main(void)
{
    unity_run_menu();
}
