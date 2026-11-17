/* gens64: serial (SPI) EEPROM of the T-5740 board (Pier Solar), from Genesis Plus GX
   (core/cart_hw/eeprom_spi.c, Eke-Eke).
   The memory is the 64 KB SRAM of Gens (.srm, savestates); the serial state is saved by
   EEPROM_SPI_Save/Load. Lines of the board (data written to A13009): bit 0 SI, bit 1 SCK,
   bit 2 !HOLD, bit 3 !CS; read: bit 0 SO. */

#include <string.h>
#include "eeprom_spi.h"

extern unsigned char SRAM[64 * 1024];

#define SIZE_MASK 0xffff
#define PAGE_MASK 0x7f

#define BIT_DATA (0)
#define BIT_CLK  (1)
#define BIT_HOLD (2)
#define BIT_CS   (3)

enum { STANDBY, GET_OPCODE, GET_ADDRESS, WRITE_BYTE, READ_BYTE };

static struct
{
	unsigned char cs, clk, out, status, opcode, buffer;
	unsigned short addr;
	unsigned char cycles, state;
} spi;

void EEPROM_SPI_Init(void)
{
	memset(&spi, 0, sizeof(spi));
	spi.out = 1;
	spi.state = GET_OPCODE;
}

void EEPROM_SPI_Write(unsigned int data)
{
	if (data & (1 << BIT_HOLD))
	{
		if (data & (1 << BIT_CS))
		{
			/* !CS high: end of the operation */
			spi.cycles = 0;
			spi.out = 1;
			spi.opcode = 0;
			spi.state = GET_OPCODE;
		}
		else if ((data & (1 << BIT_CLK)) && !spi.clk)	/* SCK rising edge */
		{
			switch (spi.state)
			{
			case GET_OPCODE:
				spi.opcode |= (data >> BIT_DATA) & 1;
				if (++spi.cycles == 8)
				{
					spi.cycles = 0;
					switch (spi.opcode)
					{
					case 0x01:	/* WRITE STATUS */
						spi.buffer = 0;
						spi.state = WRITE_BYTE;
						break;
					case 0x02:	/* WRITE */
					case 0x03:	/* READ */
						spi.addr = 0;
						spi.state = GET_ADDRESS;
						break;
					case 0x04:	/* WRITE DISABLE */
						spi.status &= ~0x02;
						spi.state = STANDBY;
						break;
					case 0x05:	/* READ STATUS */
						spi.buffer = spi.status;
						spi.state = READ_BYTE;
						break;
					case 0x06:	/* WRITE ENABLE */
						spi.status |= 0x02;
						spi.state = STANDBY;
						break;
					default:
						spi.state = STANDBY;
						break;
					}
				}
				else
					spi.opcode <<= 1;
				break;

			case GET_ADDRESS:
				spi.addr |= (data >> BIT_DATA) & 1;
				if (++spi.cycles == 16)
				{
					spi.cycles = 0;
					spi.addr &= SIZE_MASK;
					if (spi.opcode & 0x01)
					{
						spi.buffer = SRAM[spi.addr];
						spi.state = READ_BYTE;
					}
					else
					{
						spi.buffer = 0;
						spi.state = WRITE_BYTE;
					}
				}
				else
					spi.addr <<= 1;
				break;

			case WRITE_BYTE:
				spi.buffer |= (data >> BIT_DATA) & 1;
				if (++spi.cycles == 8)
				{
					spi.cycles = 0;
					if (spi.opcode & 0x01)
					{
						spi.status = (spi.status & 0x02) | (spi.buffer & 0x0c);
						spi.state = STANDBY;
					}
					else
					{
						if (spi.status & 2)
						{
							/* block protection bits BP0, BP1 */
							switch ((spi.status >> 2) & 0x03)
							{
							case 0x01: if (spi.addr < 0xC000) SRAM[spi.addr] = spi.buffer; break;
							case 0x02: if (spi.addr < 0x8000) SRAM[spi.addr] = spi.buffer; break;
							case 0x03: break;
							default: SRAM[spi.addr] = spi.buffer; break;
							}
						}
						spi.buffer = 0;
						/* sequential writes stay within the page */
						spi.addr = (unsigned short)((spi.addr & ~PAGE_MASK) | ((spi.addr + 1) & PAGE_MASK));
					}
				}
				else
					spi.buffer <<= 1;
				break;

			case READ_BYTE:
				spi.out = (spi.buffer >> (7 - spi.cycles)) & 1;
				if (++spi.cycles == 8)
				{
					spi.cycles = 0;
					if (spi.opcode == 0x03)
					{
						spi.addr = (spi.addr + 1) & SIZE_MASK;
						spi.buffer = SRAM[spi.addr];
					}
				}
				break;

			default:
				break;
			}
		}
	}
	spi.cs = (data >> BIT_CS) & 1;
	spi.clk = (data >> BIT_CLK) & 1;
}

unsigned int EEPROM_SPI_Read(void)
{
	return (unsigned int)spi.out << BIT_DATA;
}

void EEPROM_SPI_Save(unsigned char *d)
{
	memset(d, 0, EEPROM_SPI_STATE_SIZE);
	d[0] = 'E';
	d[1] = spi.cs;
	d[2] = spi.clk;
	d[3] = spi.out;
	d[4] = spi.status;
	d[5] = spi.opcode;
	d[6] = spi.buffer;
	d[7] = (unsigned char)spi.addr;
	d[8] = (unsigned char)(spi.addr >> 8);
	d[9] = spi.cycles;
	d[10] = spi.state;
}

void EEPROM_SPI_Load(const unsigned char *d)
{
	if (d[0] != 'E')
	{
		EEPROM_SPI_Init();
		return;
	}
	spi.cs = d[1];
	spi.clk = d[2];
	spi.out = d[3];
	spi.status = d[4];
	spi.opcode = d[5];
	spi.buffer = d[6];
	spi.addr = (unsigned short)(d[7] | d[8] << 8);
	spi.cycles = d[9];
	spi.state = d[10] <= READ_BYTE ? d[10] : STANDBY;
}
