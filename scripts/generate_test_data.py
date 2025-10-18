import pandas as pd
import numpy as np
from datetime import datetime

def generate_random_tick_data_fast(num_ticks, start_time, base_price, price_std_dev, max_volume):
    """
    Generates random tick data using a fast, vectorized approach.

    :param num_ticks: Number of ticks to generate.
    :param start_time: The starting datetime for the data.
    :param base_price: The initial price for the ticks.
    :param price_std_dev: Standard deviation for the random price changes.
    :param max_volume: The maximum volume for a single tick.
    :return: A pandas DataFrame with the generated data.
    """
    # Generate all price changes at once and create a random walk with cumsum()
    price_changes = np.random.normal(0, price_std_dev, size=num_ticks)
    prices = base_price + np.cumsum(price_changes)
    # Apply the floor to all values. This is much faster than checking in a loop.
    prices = np.maximum(0.01, prices)

    # Generate all time delays, get the cumulative sum, and create timestamps
    delays_ms = np.random.randint(1, 101, size=num_ticks)
    cumulative_delays_ms = np.cumsum(delays_ms)
    # Use pandas' fast to_timedelta to convert all at once
    timestamps = pd.to_datetime(start_time) + pd.to_timedelta(cumulative_delays_ms, unit='ms')

    # Generate volumes (already vectorized)
    volumes = np.random.randint(1, max_volume + 1, size=num_ticks)

    # Create the DataFrame
    data = pd.DataFrame({
        'timestamp': timestamps,
        'price': prices,
        'volume': volumes
    })

    return data

# --- Main script execution ---
NUM_TICKS = 1000000
START_TIME = datetime(2023, 10, 26, 9, 30, 0)
BASE_PRICE = 100.00
PRICE_STD_DEV = 0.5
MAX_VOLUME = 500
FILE_NAME = 'random_tick_data.csv'

tick_data = generate_random_tick_data_fast(
    NUM_TICKS,
    START_TIME,
    BASE_PRICE,
    PRICE_STD_DEV,
    MAX_VOLUME
)

try:
    tick_data.to_csv(FILE_NAME, index=False)
    print(f"Successfully generated and saved {NUM_TICKS} ticks to {FILE_NAME}")
    print("\nFirst 5 rows of the generated data:")
    print(tick_data.head())
except Exception as e:
    print(f"An error occurred while saving the file: {e}")