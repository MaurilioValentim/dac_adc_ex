//
// Included Files
//
#include "driverlib.h"
#include "device.h"
#include "board.h"
#include <math.h>
#include <stdint.h>

// Pinout 30 DAC

//
// Defines
//
#define NUM_HARMONICS      (20u)
#define PI_F               (3.14159265358979323846f)

#define TS                 (100e-6f)     // 100 us
#define DAC_BITS           (12u)
#define DAC_MAX_CODE       ((1u << DAC_BITS) - 1u)   // 4095
#define DAC_VREF           (3.3f)
#define DAC_OFFSET_VOLTS   (1.65f)       // metade de 3.3V (mid-scale bias)


// ADC -> Corrente

// Resolução do ADC
#define ADC_MAX_VALUE          (4095.0f)

// Offset do sensor (0 A)
#define ADC_CURRENT_OFFSET     (2080.0f)

// Faixa de corrente
#define CURRENT_MIN            (-90.0f)
#define CURRENT_MAX            ( 90.0f)

// Ganho (A por bit)
#define CURRENT_GAIN           ((CURRENT_MAX - CURRENT_MIN) / ADC_MAX_VALUE)

// Variaveis Globais

volatile uint16_t dac_code = 0;

volatile uint16_t adc_code = 0;

volatile float i_afse  = 0.0f; 
volatile float nm = 1250.0f;  // RPM (exemplo)


//
// Harmonics tables (float para rodar r�pido no DSP)
//
static const float amplitude[NUM_HARMONICS] =
{
    1.0000000000f, 0.0166854781f, 0.2313161489f, 0.0122283770f, 0.1392132310f,
    0.0044287539f, 0.0600936651f, 0.0059369796f, 0.0052418003f, 0.0036563083f,
    0.0098227707f, 0.0061294503f, 0.0830198452f, 0.0054446229f, 0.0450157865f,
    0.0143079677f, 0.0581947390f, 0.0017736714f, 0.0380421840f, 1.3127838354f
};

static const float phase[NUM_HARMONICS] =
{
   -2.9731804911f, -2.9591813574f, -2.6156732113f, -2.6025044232f,  0.8319285773f,
    1.1614589709f,  1.1765511595f,  1.2944368828f,  1.5156543811f,  1.6169336163f,
   -1.3190662434f,  1.9996525388f, -0.9478240417f, -0.8664741389f, -0.5955124705f,
   -0.4810861834f,  2.7399890355f,  0.9861333051f,  3.1233893040f,  2.9577095114f
};

//
// Estado interno
//
static volatile float t_s = 0.0f;

//
// Prot�tipos
//
static inline float clampf(float x, float xmin, float xmax);
static inline uint16_t volt_to_dac_code(float v_out_volts);
static inline float positive_only(float x);

static void UpdateVcontrolAndWriteDAC(float nm, float i_afse, float dac_gain);

__interrupt void INT_myCPUTIMER1_ISR(void);

//
// Main
//
void main(void)
{
    //
    // Device Initialization
    //
    Device_init();

    //
    // Initializes PIE and clears PIE registers. Disables CPU interrupts.
    //
    Interrupt_initModule();

    //
    // Initializes the PIE vector table with pointers to the shell ISR.
    //
    Interrupt_initVectorTable();

    //
    // Board init (Timer/DAC/Interrupt mapping vem do SysConfig/board.c)
    //
    Board_init();

    //
    // Enable Global Interrupt (INTM) and realtime interrupt (DBGM)
    //
    EINT;
    ERTM;
    DAC_setShadowValue(DAC0_BASE, 2048);
    //
    // Loop principal vazio: tudo roda no ISR do CPUTIMER1
    //
    while(1)
    {
        NOP;
    }
}

//
// --------------------------------------
// Utilit�rios
// --------------------------------------
//
static inline float clampf(float x, float xmin, float xmax)
{
    if (x < xmin) return xmin;
    if (x > xmax) return xmax;
    return x;
}

static inline float positive_only(float x)
{
    return (x > 0.0f) ? x : 0.0f;
}

static inline uint16_t volt_to_dac_code(float v_out_volts)
{
    // Converte tens�o (V) -> c�digo do DAC (0..4095)
    v_out_volts = clampf(v_out_volts, 0.0f, DAC_VREF);

    float code_f = (v_out_volts / DAC_VREF) * (float)DAC_MAX_CODE;

    if (code_f < 0.0f) code_f = 0.0f;
    if (code_f > (float)DAC_MAX_CODE) code_f = (float)DAC_MAX_CODE;

    return (uint16_t)(code_f + 0.5f); // arredondamento
}

//
// --------------------------------------
// C�lculo + escrita no DAC (chamar a cada TS)
// - Offset fixo em 1.65V
// - Apenas valores positivos sobem acima do offset
// --------------------------------------
//
static void UpdateVcontrolAndWriteDAC(float nm, float i_afse, float dac_gain)
{
    //
    // 1) Atualiza tempo
    //
    t_s += TS;

    // evita crescer infinito
    if (t_s > 10.0f)
    {
        t_s -= 10.0f;
    }

    //
    // 2) Calcula vT e freq
    //
    // vT = 0.0982 * nm * atan(0.016*i_afse) - 2.119
    const float vT   = (0.0982f * nm * atanf(0.016f * i_afse)) - 2.119f;
    const float freq = (nm * 8.0f) / 120.0f; // Hz

    //
    // 3) Soma harm�nicos (mantendo k += 2 como voc� pediu)
    //
    float value = 0.0f;
    const float w = 2.0f * PI_F * freq;

    for (uint16_t k = 0u; k < NUM_HARMONICS; k = (uint16_t)(k + 2u))
    {
        const float harm = (float)(k + 1u);
        value += amplitude[k] * cosf((w * harm * t_s) + phase[k]);
    }

    //
    // 4) vcontrol
    //
    const float vcontrol = (sqrtf(2.0f) * vT * value);

    //
    // 5) Sa�da para DAC:
    //    v_dac = 1.65 + gain*max(vcontrol,0)
    //
    float v_dac = DAC_OFFSET_VOLTS + (dac_gain * vcontrol);
    
    //
    // 6) Converte e escreve no DAC0
    //
    dac_code = volt_to_dac_code(v_dac);
    DAC_setShadowValue(DAC0_BASE, dac_code);
    
}

//
// --------------------------------------
// ISR do CPUTIMER1 (deve estar configurado no SysConfig/Board)
// --------------------------------------
//
__interrupt void INT_myCPUTIMER1_ISR(void)
{
    adc_code = ADC_readResult(ADC0_RESULT_BASE, ADC0_SOC0);

    
    i_afse = ((float)adc_code - ADC_CURRENT_OFFSET) * CURRENT_GAIN;
    

    //
    // Ganho do DAC:
    // - Ajuste para usar o range 1.65..3.3V sem saturar
    // - Ex.: se vcontrol_max ~ X, escolha dac_gain = (1.65 / X)
    //
    const float dac_gain = 0.1f;    // ajuste conforme sua escala

    UpdateVcontrolAndWriteDAC(nm, i_afse, dac_gain);

    //
    // Limpa ack do grupo do interrupt (ajuste se seu board usar outro grupo)
    //
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP1);
}
