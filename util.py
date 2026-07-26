import os

try:
    import settings as local_settings
except Exception:
    local_settings = None


def _get_setting(name: str, default: str = "") -> str:
    if local_settings and hasattr(local_settings, name):
        value = getattr(local_settings, name)
        if isinstance(value, str) and value.strip():
            normalized = value.strip()
            if normalized.startswith("REPLACE_WITH_"):
                return os.getenv(name, default)
            return normalized
    return os.getenv(name, default)


OPENAI_API_KEY = _get_setting("OPENAI_API_KEY", "")
GOOGLE_API_KEY = _get_setting("GOOGLE_API_KEY", "")
YUNWU_API_KEY = _get_setting("YUNWU_API_KEY", "") or OPENAI_API_KEY or GOOGLE_API_KEY
YUNWU_API_BASE_URL = _get_setting("YUNWU_API_BASE_URL", "https://yunwu.ai/v1")

SUPPORTED_PROVIDER_MODELS = [
    "gpt-5.5",
    "Claude-4.6-Sonnet",
    # "Gemini-3.5-flash",
    "gemini-3.1-flash-lite",
    "DeepSeek-V4-flash",
    "claude-haiku-4-5-20251001"
]

NON_LLM_BASELINES = [
    "Fundamental Trader",
    "Trend Follower",
    "Noise Trader",
]

DEFAULT_MODEL = _get_setting("DEFAULT_MODEL", "Gemini-3.5-flash")

MODEL_ALIASES = {
    "gpt": "gpt-5.5",
    "gpt-5.5": "gpt-5.5",
    "claude": "Claude-4.6-Sonnet",
    "claude-4.6-sonnet": "Claude-4.6-Sonnet",
    "gemini": "Gemini-3.5-flash",
    "gemini-3.5-flash": "Gemini-3.5-flash",
    "gemini-3.1-flash-lite": "gemini-3.1-flash-lite",
    "deepseek": "DeepSeek-V4-flash",
    "deepseek-v4-flash": "DeepSeek-V4-flash",
    "fundamental": "Fundamental Trader",
    "fundamental trader": "Fundamental Trader",
    "fundamental-trader": "Fundamental Trader",
    "trend": "Trend Follower",
    "trend follower": "Trend Follower",
    "trend-follower": "Trend Follower",
    "noise": "Noise Trader",
    "noise trader": "Noise Trader",
    "noise-trader": "Noise Trader",
}

MODEL_PROVIDER_IDS = {
    "gpt-5.5": "gpt-5.5",
    "Claude-4.6-Sonnet": "claude-sonnet-4-6",
    "Gemini-3.5-flash": "gemini-3.5-flash",
    "DeepSeek-V4-flash": "deepseek-v4-flash",
    "claude-haiku-4-5-20251001": "claude-haiku-4-5-20251001"
}


def resolve_model_name(model_name: str) -> str:
    normalized = (model_name or "").strip()
    if not normalized:
        normalized = DEFAULT_MODEL
    normalized = MODEL_ALIASES.get(normalized.lower(), normalized)
    return MODEL_PROVIDER_IDS.get(normalized, normalized)


def is_non_llm_baseline(model_name: str) -> bool:
    normalized = resolve_model_name(model_name)
    return normalized in NON_LLM_BASELINES


def ideal_price_midpoint(stock: str, day: int) -> float:
    stock = (stock or "").upper()
    if stock not in IDEAL_PRICE_UPPER or stock not in IDEAL_PRICE_LOWER:
        return float(STOCK_INITIAL_PRICE.get(stock, 0.0))

    d = int(day) if day is not None else IDEAL_PRICE_DAYS[0]
    mids = [
        (IDEAL_PRICE_UPPER[stock][i] + IDEAL_PRICE_LOWER[stock][i]) / 2.0
        for i in range(len(IDEAL_PRICE_DAYS))
    ]
    days = IDEAL_PRICE_DAYS

    if d <= days[0]:
        return float(mids[0])
    if d >= days[-1]:
        return float(mids[-1])

    for i in range(1, len(days)):
        left_day = days[i - 1]
        right_day = days[i]
        if d <= right_day:
            left_price = mids[i - 1]
            right_price = mids[i]
            span = right_day - left_day
            ratio = 0.0 if span <= 0 else (d - left_day) / span
            return float(left_price + ratio * (right_price - left_price))
    return float(mids[-1])


def get_yunwu_v1_base_url() -> str:
    base = YUNWU_API_BASE_URL.rstrip("/")
    return base if base.endswith("/v1") else f"{base}/v1"


def get_yunwu_root_url() -> str:
    base = get_yunwu_v1_base_url()
    return base[:-3] if base.endswith("/v1") else base


def expand_experiment_seeds(total_runs, base_seeds=None):
    if total_runs <= 0:
        return []
    seeds = list(base_seeds or DEFAULT_EXPERIMENT_BASE_SEEDS)
    if not seeds:
        seeds = [42]
    expanded = []
    for i in range(total_runs):
        base = seeds[i % len(seeds)]
        bump = (i // len(seeds)) * 10007
        expanded.append(base + bump)
    return expanded


def build_run_seed_pairs(total_runs, base_seeds=None):
    seeds = expand_experiment_seeds(total_runs, base_seeds=base_seeds)
    return [(run_idx, seed) for run_idx, seed in enumerate(seeds, start=1)]


# 实验默认运行次数与随机种子
DEFAULT_EXPERIMENT_BASE_SEEDS = [42, 128, 256, 512, 1024]
DEFAULT_EXPERIMENT_SEEDS = list(DEFAULT_EXPERIMENT_BASE_SEEDS)
DEFAULT_EXPERIMENT_SEEDS_CSV = ",".join(str(seed) for seed in DEFAULT_EXPERIMENT_SEEDS)

DEFAULT_RQ12_RUNS = 3
DEFAULT_RQ3_RUNS = 3

# 显式列出每次运行对应的随机数种子，方便直接在本文件中查看和修改。
DEFAULT_RQ12_RUN_SEEDS = expand_experiment_seeds(DEFAULT_RQ12_RUNS)
DEFAULT_RQ12_RUN_SEEDS_CSV = ",".join(str(seed) for seed in DEFAULT_RQ12_RUN_SEEDS)
DEFAULT_RQ12_RUN_SEED_MAP = build_run_seed_pairs(DEFAULT_RQ12_RUNS)

DEFAULT_RQ3_RUN_SEEDS = expand_experiment_seeds(DEFAULT_RQ3_RUNS)
DEFAULT_RQ3_RUN_SEEDS_CSV = ",".join(str(seed) for seed in DEFAULT_RQ3_RUN_SEEDS)
DEFAULT_RQ3_RUN_SEED_MAP = build_run_seed_pairs(DEFAULT_RQ3_RUNS)

# 基础设置
AGENTS_NUM = 50  # 交易员数量（RQ1）
TOTAL_DATE = 10  # 模拟时长（RQ1: 10 trading days）
TOTAL_SESSION = 3  # 每日交易次数

# 股票设置（论文口径：A/B/C/D）
STOCK_NAMES = ["A", "B", "C", "D"]

# Table 10: ideal stock price bounds (D1, D12, D78, D144, D210)
IDEAL_PRICE_DAYS = [1, 12, 78, 144, 210]
IDEAL_PRICE_UPPER = {
    "A": [27.33, 48.18, 34.38, 49.34, 47.48],
    "B": [44.16, 42.16, 42.24, 42.01, 42.97],
    "C": [249.78, 222.34, 369.86, 333.01, 307.97],
    "D": [7.74, 7.58, 12.07, 8.12, 11.37],
}
IDEAL_PRICE_LOWER = {
    "A": [26.24, 45.70, 32.85, 46.80, 45.00],
    "B": [40.29, 38.52, 38.49, 38.28, 39.05],
    "C": [234.86, 211.90, 350.05, 314.41, 291.85],
    "D": [7.53, 7.38, 11.73, 7.87, 11.95],
}

# Use midpoint of D1 bounds as initial price.
STOCK_INITIAL_PRICE = {
    stock: (IDEAL_PRICE_UPPER[stock][0] + IDEAL_PRICE_LOWER[stock][0]) / 2.0
    for stock in STOCK_NAMES
}
STOCK_A_INITIAL_PRICE = STOCK_INITIAL_PRICE["A"]
STOCK_B_INITIAL_PRICE = STOCK_INITIAL_PRICE["B"]
STOCK_C_INITIAL_PRICE = STOCK_INITIAL_PRICE["C"]
STOCK_D_INITIAL_PRICE = STOCK_INITIAL_PRICE["D"]

# Table 3/5/7/9: financial constants
COMPANY_FINANCIAL_CONSTANTS = {
    "A": {
        "cost_of_debts": [0.06, 0.05, 0.05],
        "cost_of_equity": [0.09, 0.09, 0.09],
        "debt_cost_ratio": [0.05, 0.05, 0.05],
        "equity_cost_ratio": [0.95, 0.95, 0.95],
        "wacc": [0.0885, 0.0878, 0.0880],
        "sustainable_growth_rate": [0.05, 0.05, 0.05],
        "number_of_shares": 2_000_000,
    },
    "B": {
        "cost_of_debts": [0.06, 0.05, 0.05],
        "cost_of_equity": [0.09, 0.09, 0.09],
        "debt_cost_ratio": [0.07, 0.07, 0.07],
        "equity_cost_ratio": [0.93, 0.93, 0.93],
        "wacc": [0.0879, 0.0869, 0.0872],
        "sustainable_growth_rate": [0.05, 0.05, 0.05],
        "number_of_shares": 1_000_000,
    },
    "C": {
        "cost_of_debts": [0.06, 0.05, 0.05],
        "cost_of_equity": [0.09, 0.09, 0.09],
        "debt_cost_ratio": [0.42, 0.42, 0.42],
        "equity_cost_ratio": [0.58, 0.58, 0.58],
        "wacc": [0.0774, 0.0711, 0.0732],
        "sustainable_growth_rate": [0.055, 0.055, 0.055],
        "number_of_shares": 1_200_000,
    },
    "D": {
        "cost_of_debts": [0.06, 0.05, 0.05],
        "cost_of_equity": [0.09, 0.09, 0.09],
        "debt_cost_ratio": [0.56, 0.56, 0.56],
        "equity_cost_ratio": [0.44, 0.44, 0.44],
        "wacc": [0.0732, 0.0648, 0.0676],
        "sustainable_growth_rate": [0.04, 0.04, 0.04],
        "number_of_shares": 2_700_000,
    },
}

# agent初始财产
MAX_INITIAL_PROPERTY = 5000000.0
MIN_INITIAL_PROPERTY = 100000.0

# 贷款
LOAN_TYPE = ["one-month", "two-month", "three-month"]
LOAN_TYPE_DATE = [22, 44, 66]  # 贷款时长
LOAN_RATE = [0.027, 0.03, 0.033]  # 贷款利率
REPAYMENT_DAYS = [22, 44, 66, 88, 110, 132, 154, 176, 198, 220, 242, 264]  # 付息日

# 财报
SEASONAL_DAYS = 66  # 一季度的时间
SEASON_REPORT_DAYS = [12, 78, 144, 210]  # 财报发布时间
FINANCIAL_REPORT_A = [
    "Last quarter's financial report of Company A. Revenue growth rate (YoY): 9.49%, Revenue million: 4483.99, Gross margin: 41.05%, Income Tax as a percentage of Revenue: 11.31%, Selling Expense Rate: 6.83%, Management Expense Rate: 3.83%, Net profit million: 856.6705, Depreciation and Amortization: 0.91%, Capital Expenditures: 2.30%, Changes in working capital: 0.82%, Cash Flow(million): 756.7537",
    "Last quarter's financial report of Company A. Revenue growth rate (YoY): 7.38%, Revenue million: 4417.79, Gross margin: 35.68%, Income Tax as a percentage of Revenue: 11.75%, Selling Expense Rate: 8.13%, Management Expense Rate: 4.62%, Net profit million: 493.9451, Depreciation and Amortization: 1.34%, Capital Expenditures: 2.68%, Changes in working capital: 0.86%, Cash Flow(million): 396.5329",
    "Last quarter's financial report of Company A. Revenue growth rate (YoY): 8.70%, Revenue million: 4041.30, Gross margin: 37.45%, Income Tax as a percentage of Revenue: 9.34%, Selling Expense Rate: 6.79%, Management Expense Rate: 3.41%, Net profit million: 724.3648, Depreciation and Amortization: 1.27%, Capital Expenditures: 2.44%, Changes in working capital: 0.94%, Cash Flow(million): 639.5329",
    "Last quarter's financial report of Company A. Revenue growth rate (YoY): 7.75%, Revenue million: 5024.04, Gross margin: 42.47%, Income Tax as a percentage of Revenue: 10.67%, Selling Expense Rate: 6.56%, Management Expense Rate: 4.72%, Net profit million: 1031.214, Depreciation and Amortization: 1.08%, Capital Expenditures: 2.71%, Changes in working capital: 0.08%, Cash Flow(million): 945.5034",
]
FINANCIAL_REPORT_B = [
    "Last quarter's financial report of Company B. Revenue growth rate (YoY): 19.96%, Revenue million: 1319.94, Gross margin: 31.21%, Income Tax as a percentage of Revenue: 0.70%, Selling Expense Rate: 4.69%, Management Expense Rate: 8.78%, Net profit million: 224.9179, Depreciation and Amortization: 1.13%, Capital Expenditures: 1.77%, Changes in working capital: 0.59%, Cash Flow(million): 208.7266",
    "Last quarter's financial report of Company B. Revenue growth rate (YoY): 19.86%, Revenue million: 1096.70, Gross margin: 31.26%, Income Tax as a percentage of Revenue: 0.71%, Selling Expense Rate: 3.62%, Management Expense Rate: 9.90%, Net profit million: 186.7678, Depreciation and Amortization: 0.67%, Capital Expenditures: 1.44%, Changes in working capital: -0.31%, Cash Flow(million): 181.6862",
    "Last quarter's financial report of Company B. Revenue growth rate (YoY): 18.21%, Revenue million: 1676.70, Gross margin: 31.58%, Income Tax as a percentage of Revenue: 0.92%, Selling Expense Rate: 3.78%, Management Expense Rate: 10.27%, Net profit million: 278.3327, Depreciation and Amortization: 0.77%, Capital Expenditures: 1.56%, Changes in working capital: -0.06%, Cash Flow(million): 266.1486",
    "Last quarter's financial report of Company B. Revenue growth rate (YoY): 15.98%, Revenue million: 1075.13, Gross margin: 32.41%, Income Tax as a percentage of Revenue: 1.08%, Selling Expense Rate: 3.79%, Management Expense Rate: 10.70%, Net profit million: 181.1602, Depreciation and Amortization: 1.09%, Capital Expenditures: 2.28%, Changes in working capital: 0.67%, Cash Flow(million): 161.1985",
]
FINANCIAL_REPORT_C = [
    "Last quarter's financial report of Company C. Revenue million: 40123.56, Net profit million: 10789.654321, Cash Flow(million): 10123.456789, WACC: 7.74%, Sustainable growth rate: 5.5%, Debt cost ratio: 42%, Equity cost ratio: 58%.",
    "Last quarter's financial report of Company C. Revenue million: 41867.90, Net profit million: 11234.567890, Cash Flow(million): 10678.901234, WACC: 7.11%, Sustainable growth rate: 5.5%, Debt cost ratio: 42%, Equity cost ratio: 58%.",
    "Last quarter's financial report of Company C. Revenue million: 40567.12, Net profit million: 9876.543210, Cash Flow(million): 9324.567890, WACC: 7.32%, Sustainable growth rate: 5.5%, Debt cost ratio: 42%, Equity cost ratio: 58%.",
    "Last quarter's financial report of Company C. Revenue million: 42234.78, Net profit million: 10345.678901, Cash Flow(million): 9789.012345, WACC: 7.32%, Sustainable growth rate: 5.5%, Debt cost ratio: 42%, Equity cost ratio: 58%.",
]
FINANCIAL_REPORT_D = [
    "Last quarter's financial report of Company D. Revenue million: 17989.45, Net profit million: 1723.456789, Cash Flow(million): 1612.345678, WACC: 7.32%, Sustainable growth rate: 4.0%, Debt cost ratio: 56%, Equity cost ratio: 44%.",
    "Last quarter's financial report of Company D. Revenue million: 17834.56, Net profit million: 2015.678901, Cash Flow(million): 1901.234567, WACC: 6.48%, Sustainable growth rate: 4.0%, Debt cost ratio: 56%, Equity cost ratio: 44%.",
    "Last quarter's financial report of Company D. Revenue million: 17789.01, Net profit million: 1934.567890, Cash Flow(million): 1823.456789, WACC: 6.76%, Sustainable growth rate: 4.0%, Debt cost ratio: 56%, Equity cost ratio: 44%.",
    "Last quarter's financial report of Company D. Revenue million: 17845.67, Net profit million: 1712.345678, Cash Flow(million): 1601.234567, WACC: 6.76%, Sustainable growth rate: 4.0%, Debt cost ratio: 56%, Equity cost ratio: 44%.",
]

# 特殊事件
EVENT_1_DAY = 78
EVENT_1_MESSAGE = (
    "The government has announced a reduction in the reserve requirement ratio. "
    "The lending interest rates have been lowered."
)
EVENT_1_LOAN_RATE = [0.024, 0.027, 0.030]  # 降准后的利率放在这里

EVENT_2_DAY = 144
EVENT_2_MESSAGE = "The government has announced an increase in interest rates."
EVENT_2_LOAN_RATE = [0.0255, 0.0285, 0.0315]
