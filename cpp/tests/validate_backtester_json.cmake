if(NOT DEFINED BACKTESTER)
    message(FATAL_ERROR "BACKTESTER executable path is required")
endif()

execute_process(
    COMMAND
        "${BACKTESTER}"
        --events 6000
        --strategy trend
        --folds 3
        --final-test-percent 20
        --embargo-events 32
        --metric-bucket-events 1000000
        --seed 42
        --json
    RESULT_VARIABLE backtester_result
    OUTPUT_VARIABLE backtester_json
    ERROR_VARIABLE backtester_error
)

if(NOT backtester_result EQUAL 0)
    message(FATAL_ERROR
        "backtester failed (${backtester_result}): ${backtester_error}"
    )
endif()

string(JSON root_type
    ERROR_VARIABLE json_error
    TYPE "${backtester_json}"
)
if(json_error OR NOT root_type STREQUAL "OBJECT")
    message(FATAL_ERROR "backtester emitted invalid JSON: ${json_error}")
endif()

string(JSON schema_version GET "${backtester_json}" schema_version)
if(NOT schema_version EQUAL 2)
    message(FATAL_ERROR
        "unexpected backtest report schema: ${schema_version}"
    )
endif()

string(JSON recommended_outcome
    GET "${backtester_json}" recommended_outcome
)
if(NOT recommended_outcome STREQUAL "trend")
    message(FATAL_ERROR
        "strategy filter was not preserved: ${recommended_outcome}"
    )
endif()

string(JSON final_test_type
    TYPE "${backtester_json}" recommended_outcome_final_test
)
if(NOT final_test_type STREQUAL "OBJECT")
    message(FATAL_ERROR "final_test metrics object is missing")
endif()

foreach(metric_name
        mean_bucket_pnl_ticks
        bucket_volatility_ticks
        downside_deviation_ticks
        worst_bucket_pnl_ticks)
    string(JSON metric_type
        TYPE "${backtester_json}"
        recommended_outcome_final_test "${metric_name}"
    )
    if(NOT metric_type STREQUAL "NULL")
        message(FATAL_ERROR
            "${metric_name} must be null with zero complete buckets"
        )
    endif()
endforeach()

execute_process(
    COMMAND
        "${BACKTESTER}"
        --events 6000
        --strategy trend
        --folds 3
        --final-test-percent 20
        --embargo-events 32
        --fixed-fee-ticks 100000
        --seed 42
        --json
    RESULT_VARIABLE expensive_result
    OUTPUT_VARIABLE expensive_json
    ERROR_VARIABLE expensive_error
)
if(NOT expensive_result EQUAL 0)
    message(FATAL_ERROR
        "high-cost backtester failed (${expensive_result}): "
        "${expensive_error}"
    )
endif()

string(JSON expensive_outcome
    GET "${expensive_json}" recommended_outcome
)
if(NOT expensive_outcome STREQUAL "no_trade")
    message(FATAL_ERROR
        "high-cost catalog should recommend no_trade: "
        "${expensive_outcome}"
    )
endif()

string(JSON expensive_final_pnl
    GET "${expensive_json}"
    recommended_outcome_final_test net_liquidation_pnl_ticks
)
if(NOT expensive_final_pnl EQUAL 0)
    message(FATAL_ERROR
        "no_trade final-test P&L must be zero: ${expensive_final_pnl}"
    )
endif()
