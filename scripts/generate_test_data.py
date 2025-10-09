import pandas as pd
import numpy as np
from datetime import datetime, timedelta

def generate_random_tick_data(num_ticks, start_time, base_price, price_std_dev, max_volume):
    """
    Generates random tick data for a specified number of ticks.

    :param num_ticks: Number of ticks to generate.
    :param start_time: The starting datetime for the data.
    :param base_price: The initial price for the ticks.
    :param price_std_dev: Standard deviation for the random price changes.
    :param max_volume: The maximum volume for a single tick.
    :return: A pandas DataFrame with the generated data.
    """
    timestamps = []
    prices = []
    volumes = []

    current_time = start_time
    for i in range(num_ticks):
        delay_ms = np.random.randint(1, 101)
        current_time += timedelta(milliseconds=delay_ms)
        timestamps.append(current_time)

    price = base_price
    for i in range(num_ticks):
        price_change = np.random.normal(0, price_std_dev)
        price += price_change

        prices.append(max(0.01, price))

    volumes = np.random.randint(1, max_volume + 1, size=num_ticks)

    data = pd.DataFrame({
        'timestamp': timestamps,
        'price': prices,
        'volume': volumes
    })

    return data

NUM_TICKS = 100000
START_TIME = datetime(2023, 10, 26, 9, 30, 0)
BASE_PRICE = 100.00
PRICE_STD_DEV = 0.05
MAX_VOLUME = 500
FILE_NAME = 'random_tick_data.csv'

tick_data = generate_random_tick_data(
    NUM_TICKS,
    START_TIME,
    BASE_PRICE,
    PRICE_STD_DEV,
    MAX_VOLUME
)

tick_data = tick_data.sort_values(by='timestamp').reset_index(drop=True)

try:
    tick_data.to_csv(FILE_NAME, index=False)
    print(f"Successfully generated and saved {NUM_TICKS} ticks to {FILE_NAME}")
    print("\nFirst 5 rows of the generated data:")
    print(tick_data.head())
except Exception as e:
    print(f"An error occurred while saving the file: {e}")