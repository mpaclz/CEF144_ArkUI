//
// Created on 2026/6/6.
//
// Node APIs are not fully supported. To solve the compilation error of the interface cannot be found,
// please include "napi/native_api.h".

#ifndef OPENQTCEF_CALLBACKWRAPPER_H
#define OPENQTCEF_CALLBACKWRAPPER_H

#include <node_api.h>

#include <tuple>
#include <string>
#include <memory>
#include <utility>
#include <type_traits>
#include <cstdint>

template<typename... Args>
class JsThreadSafeCallback {
public:
    JsThreadSafeCallback() = default;

    ~JsThreadSafeCallback()
    {
        Release();
    }

    JsThreadSafeCallback(const JsThreadSafeCallback&) = delete;
    JsThreadSafeCallback& operator=(const JsThreadSafeCallback&) = delete;

    bool Init(
        napi_env env,
        napi_value jsFunction,
        const char* resourceName = "JsThreadSafeCallback")
    {
        if (env == nullptr || jsFunction == nullptr) {
            return false;
        }

        napi_valuetype type;
        napi_status status = napi_typeof(env, jsFunction, &type);
        if (status != napi_ok || type != napi_function) {
            return false;
        }

        napi_value resource = nullptr;
        status = napi_create_string_utf8(
            env,
            resourceName,
            NAPI_AUTO_LENGTH,
            &resource
        );
        if (status != napi_ok) {
            return false;
        }

        status = napi_create_threadsafe_function(
            env,
            jsFunction,
            nullptr,
            resource,
            0,
            1,
            nullptr,
            nullptr,
            nullptr,
            &JsThreadSafeCallback<Args...>::CallJs,
            &tsfn_
        );

        return status == napi_ok && tsfn_ != nullptr;
    }

    void Call(Args... args)
    {
        if (tsfn_ == nullptr) {
            return;
        }

        auto* data = new CallbackData(std::forward<Args>(args)...);

        napi_status status = napi_call_threadsafe_function(
            tsfn_,
            data,
            napi_tsfn_nonblocking
        );

        if (status != napi_ok) {
            delete data;
        }
    }

    bool IsValid() const
    {
        return tsfn_ != nullptr;
    }

    void Release()
    {
        if (tsfn_ != nullptr) {
            napi_release_threadsafe_function(tsfn_, napi_tsfn_release);
            tsfn_ = nullptr;
        }
    }

private:
    struct CallbackData {
        explicit CallbackData(Args... inputArgs)
            : args(std::move(inputArgs)...)
        {
        }

        std::tuple<std::decay_t<Args>...> args;
    };

private:
    static void CallJs(
        napi_env env,
        napi_value jsCallback,
        void* context,
        void* data)
    {
        CallbackData* callbackData = static_cast<CallbackData*>(data);
        if (callbackData == nullptr) {
            return;
        }

        if (env != nullptr && jsCallback != nullptr) {
            constexpr size_t argc = sizeof...(Args);
            napi_value argv[argc == 0 ? 1 : argc] = { nullptr };

            FillNapiArgs(
                env,
                callbackData->args,
                argv,
                std::index_sequence_for<Args...>{}
            );

            napi_value global = nullptr;
            napi_get_global(env, &global);

            napi_value result = nullptr;
            napi_call_function(
                env,
                global,
                jsCallback,
                argc,
                argv,
                &result
            );
        }

        delete callbackData;
    }

    template<size_t... I>
    static void FillNapiArgs(
        napi_env env,
        const std::tuple<std::decay_t<Args>...>& tupleArgs,
        napi_value* argv,
        std::index_sequence<I...>)
    {
        ((argv[I] = ToNapiValue(env, std::get<I>(tupleArgs))), ...);
    }

    static napi_value ToNapiValue(napi_env env, int32_t value)
    {
        napi_value result = nullptr;
        napi_create_int32(env, value, &result);
        return result;
    }

    static napi_value ToNapiValue(napi_env env, int64_t value)
    {
        napi_value result = nullptr;
        napi_create_int64(env, value, &result);
        return result;
    }

    static napi_value ToNapiValue(napi_env env, uint32_t value)
    {
        napi_value result = nullptr;
        napi_create_uint32(env, value, &result);
        return result;
    }

    static napi_value ToNapiValue(napi_env env, double value)
    {
        napi_value result = nullptr;
        napi_create_double(env, value, &result);
        return result;
    }

    static napi_value ToNapiValue(napi_env env, float value)
    {
        napi_value result = nullptr;
        napi_create_double(env, static_cast<double>(value), &result);
        return result;
    }

    static napi_value ToNapiValue(napi_env env, bool value)
    {
        napi_value result = nullptr;
        napi_get_boolean(env, value, &result);
        return result;
    }

    static napi_value ToNapiValue(napi_env env, const char* value)
    {
        napi_value result = nullptr;
        napi_create_string_utf8(
            env,
            value == nullptr ? "" : value,
            NAPI_AUTO_LENGTH,
            &result
        );
        return result;
    }

    static napi_value ToNapiValue(napi_env env, const std::string& value)
    {
        napi_value result = nullptr;
        napi_create_string_utf8(
            env,
            value.c_str(),
            value.size(),
            &result
        );
        return result;
    }

private:
    napi_threadsafe_function tsfn_ = nullptr;
};

template<typename... Args>
std::shared_ptr<JsThreadSafeCallback<Args...>> makeJsThreadSafeCallback(
    napi_env env,
    napi_value jsFunction,
    const char* resourceName = "JsThreadSafeCallback")
{
    auto callback = std::make_shared<JsThreadSafeCallback<Args...>>();

    if (!callback->Init(env, jsFunction, resourceName)) {
        return nullptr;
    }

    return callback;
}

#endif //OPENQTCEF_CALLBACKWRAPPER_H
