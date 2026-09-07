/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "spi.h"
#include "tim.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* ═══════════════════════════════════════════
 *  Protocol constants
 * ═══════════════════════════════════════════ */
#define FRAME_LEN 5
#define CMD_WRITE_FLAG 0x80
#define CMD_ADDR_MASK 0x7F

/* ═══════════════════════════════════════════
 *  Register addresses
 * ═══════════════════════════════════════════ */
#define REG_DEVICE_ID 0x00
#define REG_FW_VERSION 0x01
#define REG_STATUS 0x02
#define REG_CONTROL 0x03
#define REG_SAMPLE_RATE 0x04
#define REG_FIFO_LEVEL 0x05
#define REG_DATA_SEQ 0x06
#define REG_DATA_VAL 0x07
#define REG_OVERFLOW_COUNT 0x08
#define REG_SPI_REARM_FAIL 0x09
#define REG_SPI_ERROR_COUNT 0x0A
#define REG_NOP 0x7F

/* ═══════════════════════════════════════════
 *  STATUS bit definitions
 * ═══════════════════════════════════════════ */
#define ST_RUNNING (1u << 0)
#define ST_CMD_ERR (1u << 1)
#define ST_RANGE_ERR (1u << 2)
#define ST_SPI_RESYNC (1u << 3)

/* ═══════════════════════════════════════════
 *  SPI buffers
 * ═══════════════════════════════════════════ */
static uint8_t rx_buf[FRAME_LEN];
static uint8_t tx_buf[FRAME_LEN];

/* ═══════════════════════════════════════════
 *  Register state
 * ═══════════════════════════════════════════ */
static volatile uint32_t reg_status = 0;
static volatile uint32_t reg_control = 0;
static volatile uint32_t reg_sample_rate = 1000;

/* ═══════════════════════════════════════════
 *  FIFO
 * ═══════════════════════════════════════════ */
#define FIFO_DEPTH 32

typedef struct
{
  uint32_t sequence;
  uint32_t value;
} FifoItem;

static FifoItem fifo_buf[FIFO_DEPTH];
static volatile uint16_t fifo_head = 0;     /* next write slot */
static volatile uint16_t fifo_tail = 0;     /* next read slot */
static volatile uint32_t fifo_overflow = 0; /* cumulative drop count */
static volatile uint32_t seq_counter = 0;   /* global monotonic sequence number */

/* Diagnostic counter: how many times HAL_SPI_TransmitReceive_IT failed to
 * re-arm. Used to check whether "once wedged, no further data ever comes
 * in" actually holds. */
static volatile uint32_t spi_rearm_fail = 0;

/* Diagnostic counter: total number of HAL_SPI_ErrorCallback invocations.
 * Used to distinguish "CPU still alive, SPI hardware keeps erroring" from
 * "CPU/interrupts stopped responding entirely at some point" (in the
 * latter case this counter simply stops increasing). */
static volatile uint32_t spi_error_count = 0;

/* peek/pop intermediate state */
static uint32_t last_seq = 0;
static uint32_t last_val = 0;
static uint8_t item_peeked = 0;

/* ═══════════════════════════════════════════
 *  Peripheral handles (declared elsewhere by CubeMX, externed here)
 * ═══════════════════════════════════════════ */
extern SPI_HandleTypeDef hspi2;
extern TIM_HandleTypeDef htim2;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

static uint32_t unpack_u32(const uint8_t *p);
static void pack_u32(uint8_t *p, uint32_t v);
static uint32_t reg_read(uint8_t addr);
static void reg_write(uint8_t addr, uint32_t val);
static void handle_frame(void);
static void spi_resync(void);
static void update_data_ready_gpio(void);
static void update_timer_rate(uint32_t rate_hz);
static void fifo_push(uint32_t value);
static uint8_t fifo_peek(uint32_t *seq_out, uint32_t *val_out);
static uint8_t fifo_pop(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ═══════════════════════════════════════════
 *  Utility functions
 * ═══════════════════════════════════════════ */
static uint32_t unpack_u32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void pack_u32(uint8_t *p, uint32_t v)
{
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

/* ═══════════════════════════════════════════
 *  FIFO operations
 * ═══════════════════════════════════════════ */
static uint16_t fifo_level(void) {
    uint16_t h = fifo_head;
    uint16_t t = fifo_tail;
    if (h >= t) return h - t;
    return FIFO_DEPTH - t + h;
}

static void fifo_push(uint32_t value) {
    uint16_t next = (fifo_head + 1) % FIFO_DEPTH;
    if (next == fifo_tail) {
        /* full - drop the sample */
        fifo_overflow++;
        return;
    }
    fifo_buf[fifo_head].sequence = seq_counter++;
    fifo_buf[fifo_head].value    = value;
    fifo_head = next;  /* this is what makes the new item visible to the consumer */
}

static uint8_t fifo_peek(uint32_t *seq_out, uint32_t *val_out) {
    if (fifo_head == fifo_tail) return 0;
    *seq_out = fifo_buf[fifo_tail].sequence;
    *val_out = fifo_buf[fifo_tail].value;
    return 1;
}

static uint8_t fifo_pop(void) {
    if (fifo_head == fifo_tail) return 0;
    fifo_tail = (fifo_tail + 1) % FIFO_DEPTH;
    return 1;
}
/* ═══════════════════════════════════════════
 *  DATA_READY GPIO
 * ═══════════════════════════════════════════ */
static void update_data_ready_gpio(void)
{
  // if (fifo_count > 0)
  // {
  //   HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
  // }
  // else
  // {
  //   HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
  // }

      if (fifo_head != fifo_tail) {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
    } else {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
    }

}

/* ═══════════════════════════════════════════
 *  Timer rate update
 * ═══════════════════════════════════════════ */
static void update_timer_rate(uint32_t rate_hz)
{
  /*
   * base freq = 72 MHz / 7200 = 10000 Hz
   * period = 10000 / rate_hz
   * rate_hz valid range 1-10000 -> period range 1-10000
   */
  uint32_t period = 10000 / rate_hz;
  if (period < 1)
    period = 1;

  __HAL_TIM_SET_AUTORELOAD(&htim2, period - 1);
  __HAL_TIM_SET_COUNTER(&htim2, 0);
}

/* ═══════════════════════════════════════════
 *  SPI error recovery
 * ═══════════════════════════════════════════ */
static void spi_resync(void)
{
  volatile uint32_t tmp;

  __HAL_SPI_DISABLE(&hspi2);
  tmp = hspi2.Instance->DR;
  tmp = hspi2.Instance->SR;
  (void)tmp;
  __HAL_SPI_CLEAR_OVRFLAG(&hspi2);

  hspi2.State = HAL_SPI_STATE_READY;
  hspi2.ErrorCode = HAL_SPI_ERROR_NONE;
  hspi2.Lock = HAL_UNLOCKED; /* guards against the previous error path leaving
                                the lock held, which would make every future
                                HAL_SPI_TransmitReceive_IT call silently
                                return HAL_BUSY forever */

  reg_status |= ST_SPI_RESYNC;

  __HAL_SPI_ENABLE(&hspi2);
}

/* ═══════════════════════════════════════════
 *  Register read
 * ═══════════════════════════════════════════ */
static uint32_t reg_read(uint8_t addr)
{
  switch (addr)
  {
  /* -- carried over from V1.2 -- */
  case REG_DEVICE_ID:
    return 0xAC00ACC0u;

  case REG_FW_VERSION:
    return 0x00010300u; /* V1.3 -> 0x00010300 */

  case REG_STATUS:
    return reg_status;

  case REG_CONTROL:
    return reg_control;

  case REG_SAMPLE_RATE:
    return reg_sample_rate;

  case REG_NOP:
    return 0;

  /* -- new in V1.3 -- */
  case REG_FIFO_LEVEL:
    return fifo_level();

  case REG_DATA_SEQ:
    if (fifo_peek(&last_seq, &last_val))
    {
      item_peeked = 1;
      return last_seq;
    }
    return 0;

  case REG_DATA_VAL:
    if (item_peeked)
    {
      item_peeked = 0;
      fifo_pop();
      update_data_ready_gpio();
      return last_val;
    }
    if (fifo_peek(&last_seq, &last_val))
    {
      fifo_pop();
      update_data_ready_gpio();
      return last_val;
    }
    return 0;

  case REG_OVERFLOW_COUNT:
    return fifo_overflow;

  case REG_SPI_REARM_FAIL:
    return spi_rearm_fail;

  case REG_SPI_ERROR_COUNT:
    return spi_error_count;

  default:
    reg_status |= ST_CMD_ERR;
    return 0;
  }
}

/* ═══════════════════════════════════════════
 *  Register write
 * ═══════════════════════════════════════════ */
static void reg_write(uint8_t addr, uint32_t val)
{
  switch (addr)
  {
  case REG_CONTROL:
    /* CLEAR_FLAGS */
    if (val & 0x02u)
    {
      reg_status &= ~(ST_CMD_ERR | ST_RANGE_ERR | ST_SPI_RESYNC);
      val &= ~0x02u;
    }
    reg_control = val & 0x01u;

    if (reg_control & 0x01u)
    {
      /* START */
      reg_status |= ST_RUNNING;
      seq_counter = 0;
      fifo_head = 0;
      fifo_tail = 0;
      fifo_overflow = 0;
      item_peeked = 0;
      update_data_ready_gpio();
      update_timer_rate(reg_sample_rate);
      HAL_TIM_Base_Start_IT(&htim2);
    }
    else
    {
      /* STOP */
      reg_status &= ~ST_RUNNING;
      HAL_TIM_Base_Stop_IT(&htim2);
      update_data_ready_gpio();
    }
    break;

  case REG_SAMPLE_RATE:
    if (val >= 1u && val <= 10000u)
    {
      reg_sample_rate = val;
      /* if acquisition is running, apply the new rate immediately */
      if (reg_control & 0x01u)
      {
        update_timer_rate(val);
      }
    }
    else
    {
      reg_status |= ST_RANGE_ERR;
    }
    break;

  default:
    /* read-only register or undefined address */
    reg_status |= ST_CMD_ERR;
    break;
  }
}

/* ═══════════════════════════════════════════
 *  Frame handling (called from the SPI callback)
 * ═══════════════════════════════════════════ */
static void handle_frame(void)
{
  uint8_t cmd = rx_buf[0];
  uint8_t addr = cmd & CMD_ADDR_MASK;
  uint32_t resp = 0;

  if (cmd & CMD_WRITE_FLAG)
  {
    reg_write(addr, unpack_u32(&rx_buf[1]));
    resp = 0;
  }
  else
  {
    resp = reg_read(addr);
  }

  tx_buf[0] = cmd; /* ECHO */
  pack_u32(&tx_buf[1], resp);
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_SPI2_Init();
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */

  /* prepare the first tx_buf frame (what the slave replies with right after power-on) */
  tx_buf[0] = REG_NOP;
  pack_u32(&tx_buf[1], 0);

  /* DATA_READY starts low */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);

  /* V7: PA9 as a dedicated sample-produced marker pin for a logic
   * analyzer, physically adjacent to PA8 (DATA_READY) on this board's
   * header - not a CubeMX-managed pin (not in the .ioc), added here by
   * hand since GPIOA's clock is already enabled by MX_GPIO_Init() above.
   * Toggled once per sample in HAL_TIM_PeriodElapsedCallback() below,
   * right after fifo_push(). */
  {
    GPIO_InitTypeDef sample_mark_init = {0};
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_9, GPIO_PIN_RESET);
    sample_mark_init.Pin = GPIO_PIN_9;
    sample_mark_init.Mode = GPIO_MODE_OUTPUT_PP;
    sample_mark_init.Pull = GPIO_NOPULL;
    sample_mark_init.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &sample_mark_init);
  }

  /* start interrupt-mode SPI receive - returns immediately, non-blocking */
  HAL_SPI_TransmitReceive_IT(&hspi2, tx_buf, rx_buf, FRAME_LEN);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* ═══════════════════════════════════════════
 *  SPI interrupt callback: one frame's TX/RX complete
 * ═══════════════════════════════════════════ */
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
  if (hspi->Instance == SPI2)
  {
    handle_frame();
    /* immediately re-arm for the next frame */
    if (HAL_SPI_TransmitReceive_IT(&hspi2, tx_buf, rx_buf, FRAME_LEN) != HAL_OK)
    {
      spi_rearm_fail++;
    }
  }
}

/* ═══════════════════════════════════════════
 *  SPI error callback
 * ═══════════════════════════════════════════ */
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
  if (hspi->Instance == SPI2)
  {
    spi_error_count++;
    spi_resync();
    if (HAL_SPI_TransmitReceive_IT(&hspi2, tx_buf, rx_buf, FRAME_LEN) != HAL_OK)
    {
      spi_rearm_fail++;
    }
  }
}

/* ═══════════════════════════════════════════
 *  Timer interrupt callback: produce one sample
 * ═══════════════════════════════════════════ */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM2)
  {
    if (reg_control & 0x01u)
    {
      /*
       * synthesized data: use the low 16 bits of the sequence number
       * as the value - swap in a sine LUT, triangle wave, or anything
       * else you want here
       */
      uint32_t sample_value = seq_counter & 0xFFFFu;
      fifo_push(sample_value);
      update_data_ready_gpio();
      /* V7: mark the exact moment this sample was produced, for a logic
       * analyzer on PA9 - see gpio.c's USER CODE 2 block for pin init. */
      HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_9);
    }
  }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
