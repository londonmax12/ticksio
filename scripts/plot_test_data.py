import pandas as pd
import matplotlib.pyplot as plt
from datetime import datetime, timedelta
import numpy as np

def plot_tick_data_from_csv(file_path): 
    try:
        df = pd.read_csv(file_path)
    except FileNotFoundError:
        print(f"ERROR: The file '{file_path}' was not found.")
        print("Please ensure the CSV file is in the same directory as this script.")
        return
    
    df['timestamp'] = pd.to_datetime(df['timestamp'])
    df = df.sort_values(by='timestamp').reset_index(drop=True)

    num_ticks = len(df)
    if num_ticks == 0:
        print("The CSV file contains no data to plot.")
        return

    print(f"Successfully loaded {num_ticks} ticks from {file_path}.")
    
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(14, 10), sharex=True)
    plt.suptitle(f'Tick Data Analysis: Price and Volume Over Time ({num_ticks} Ticks)', fontsize=16)

    ax1.plot(df['timestamp'], df['price'], label='Price', color='blue', linewidth=1)
    ax1.set_ylabel('Price ($)')
    ax1.grid(True, linestyle='--', alpha=0.6)
    ax1.legend(loc='upper left')
    
    TICK_WIDTH_DAYS = 0.0001
    
    if num_ticks > 10000:
        
        TICK_BIN_SIZE = 100
        
        
        df_vol_agg = df.groupby(df.index // TICK_BIN_SIZE).agg({
            'timestamp': 'first',
            'volume': 'sum'
        })
        
        
        vol_width_timedelta = (df_vol_agg['timestamp'].iloc[-1] - df_vol_agg['timestamp'].iloc[0]) / len(df_vol_agg)
        
        vol_width_days = vol_width_timedelta.total_seconds() / (24 * 60 * 60) * 0.9 

        ax2.bar(df_vol_agg['timestamp'], df_vol_agg['volume'], 
                label=f'Volume (Aggregated {TICK_BIN_SIZE} Ticks)', 
                color='red', alpha=0.7, width=vol_width_days)
        print(f"Volume plotting optimized: using {len(df_vol_agg)} aggregated bins instead of {num_ticks} individual bars.")
    else:
        
        ax2.bar(df['timestamp'], df['volume'], label='Volume', color='red', alpha=0.7, width=TICK_WIDTH_DAYS)
        print("Volume plotted using raw tick data.")

    ax2.set_xlabel('Timestamp')
    ax2.set_ylabel('Volume')
    ax2.grid(axis='y', linestyle='--', alpha=0.6)
    ax2.legend(loc='upper left')
    
    fig.autofmt_xdate()
    
    plt.show()

CSV_FILE_PATH = 'random_tick_data.csv'

plot_tick_data_from_csv(CSV_FILE_PATH)
