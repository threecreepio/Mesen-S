#include "stdafx.h"
#include "GbPpu.h"
#include "GbTypes.h"
#include "EventType.h"
#include "Console.h"
#include "Gameboy.h"
#include "VideoDecoder.h"
#include "RewindManager.h"
#include "GbMemoryManager.h"
#include "NotificationManager.h"
#include "MessageManager.h"
#include "../Utilities/HexUtilities.h"
#include "../Utilities/Serializer.h"

constexpr uint16_t bwRgbPalette[4] = { 0x7FFF, 0x6318, 0x318C, 0x0000 };
constexpr uint16_t evtColors[6] = { 0x18C6, 0x294A, 0x108C, 0x4210, 0x3084, 0x1184 };

void GbPpu::Init(Console* console, Gameboy* gameboy, GbMemoryManager* memoryManager, uint8_t* vram, uint8_t* oam)
{
	_console = console;
	_gameboy = gameboy;
	_memoryManager = memoryManager;
	_vram = vram;
	_oam = oam;
	
	_state = {};
	_state.Mode = PpuMode::HBlank;
	_lastFrameTime = 0;

	_outputBuffers[0] = new uint16_t[256 * 240];
	_outputBuffers[1] = new uint16_t[256 * 240];
	memset(_outputBuffers[0], 0, 256 * 240 * sizeof(uint16_t));
	memset(_outputBuffers[1], 0, 256 * 240 * sizeof(uint16_t));
	_currentBuffer = _outputBuffers[0];

	_eventViewerBuffers[0] = new uint16_t[456 * 154];
	_eventViewerBuffers[1] = new uint16_t[456 * 154];
	memset(_eventViewerBuffers[0], 0, 456 * 154 * sizeof(uint16_t));
	memset(_eventViewerBuffers[1], 0, 456 * 154 * sizeof(uint16_t));
	_currentEventViewerBuffer = _eventViewerBuffers[0];

#ifndef USEBOOTROM
	Write(0xFF40, 0x91);
	Write(0xFF42, 0x00);
	Write(0xFF43, 0x00);
	Write(0xFF45, 0x00);
	Write(0xFF47, 0xFC);
	Write(0xFF48, 0xFF);
	Write(0xFF49, 0xFF);
	Write(0xFF4A, 0);
	Write(0xFF4B, 0);
#endif
}

GbPpu::~GbPpu()
{
}

GbPpuState GbPpu::GetState()
{
	return _state;
}

uint16_t* GbPpu::GetEventViewerBuffer()
{
	return _currentEventViewerBuffer;
}

uint16_t* GbPpu::GetPreviousEventViewerBuffer()
{
	return _currentEventViewerBuffer == _eventViewerBuffers[0] ? _eventViewerBuffers[1] : _eventViewerBuffers[0];
}

void GbPpu::Exec()
{
	if(!_state.LcdEnabled) {
		//LCD is disabled, prevent IRQs, etc.
		//Not quite correct in terms of frame pacing
		if(_gameboy->GetApuCycleCount() - _lastFrameTime > 70224) {
			//More than a full frame's worth of time has passed since the last frame, send another blank frame
			_lastFrameTime = _gameboy->GetApuCycleCount();
			SendFrame();
		}
		return;
	}

	uint8_t cyclesToRun = _memoryManager->IsHighSpeed() ? 2 : 4;
	for(int i = 0; i < cyclesToRun; i++) {
		ExecCycle();
	}
}

void GbPpu::ExecCycle()
{
	_state.Cycle++;

	PpuMode oldMode = _state.Mode;
	bool oldCoincidenceFlag = _state.LyCoincidenceFlag;

	switch(_state.Cycle) {
		case 4: {
			if(_state.Scanline < 144) {
				_spriteCount = 0;
				_prevSprite = 0;
				ChangeMode(PpuMode::OamEvaluation);
			} else if(_state.Scanline == 144) {
				ChangeMode(PpuMode::VBlank);
				_memoryManager->RequestIrq(GbIrqSource::VerticalBlank);
				SendFrame();
			}
			break;
		}

		case 84: {
			if(_state.Scanline < 144) {
				std::sort(_spriteIndexes, _spriteIndexes + _spriteCount, [=](uint8_t a, uint8_t b) {
					if(_oam[a + 1] == _oam[b + 1]) {
						return a < b;
					} else {
						return _oam[a + 1] < _oam[b + 1];
					}
				});
				std::sort(_spriteX, _spriteX + _spriteCount);
				ChangeMode(PpuMode::Drawing);
				ResetRenderer();
			}
			break;
		}

		case 456: {
			_state.Cycle = 0;
			_state.Scanline++;

			if(_state.Scanline < 144) {
				ChangeMode(PpuMode::HBlank);
			} else if(_state.Scanline == 154) {
				_state.Scanline = 0;
				ChangeMode(PpuMode::HBlank);
				_console->ProcessEvent(EventType::StartFrame);
				if(_console->IsDebugging()) {
					_currentEventViewerBuffer = _currentEventViewerBuffer == _eventViewerBuffers[0] ? _eventViewerBuffers[1] : _eventViewerBuffers[0];
				}
			}
			break;
		}
	} 

	if(_state.Mode == PpuMode::Drawing) {
		if(_drawnPixels < 160) {
			RunDrawCycle();
		} else {
			ChangeMode(PpuMode::HBlank);
		}
	} else if(_state.Mode == PpuMode::OamEvaluation) {
		RunSpriteEvaluation();
	}

	UpdateLyCoincidenceFlag();
	if(_state.Mode != oldMode || _state.LyCoincidenceFlag != oldCoincidenceFlag) {
		UpdateStatIrq();
	}

	ProcessPpuCycle();
}

void GbPpu::ProcessPpuCycle()
{
	if(_console->IsDebugging()) {
		_console->ProcessPpuCycle(_state.Scanline, _state.Cycle);
		if(_state.Mode <= PpuMode::OamEvaluation) {
			_currentEventViewerBuffer[456 * _state.Scanline + _state.Cycle] = evtColors[(int)_state.Mode];
		} else if(_prevDrawnPixels != _drawnPixels && _drawnPixels > 0) {
			uint16_t color = _currentBuffer[_state.Scanline * 256 + (_drawnPixels - 1)];
			_currentEventViewerBuffer[456 * _state.Scanline + _state.Cycle] = color;
		} else {
			_currentEventViewerBuffer[456 * _state.Scanline + _state.Cycle] = evtColors[(int)_evtColor];
		}
		_prevDrawnPixels = _drawnPixels;
	}
}

void GbPpu::RunDrawCycle()
{
	if(_state.Cycle < 89) {
		//Idle cycles
		_evtColor = EvtColor::RenderingIdle;
		return;
	}

	bool fetchWindow = _state.WindowEnabled && _drawnPixels >= _state.WindowX - 7 && _state.Scanline >= _state.WindowY;
	if(_fetchWindow != fetchWindow) {
		//Switched between window & background, reset fetcher & pixel FIFO
		_fetchWindow = fetchWindow;
		_fetchColumn = 0;

		_bgFetcher.Step = 0;
		_bgFifo.Reset();

		//Idle cycle when switching to window
		_evtColor = EvtColor::RenderingIdle;
		return;
	}

	if(_fetchSprite == -1 && _bgFifo.Size > 0) {
		if(_drawnPixels >= 0) {
			uint16_t outOffset = _state.Scanline * 256 + _drawnPixels;

			bool isSprite = false;
			GbFifoEntry entry = _bgFifo.Content[_bgFifo.Position];
			if(_oamFifo.Size > 0 && _oamFifo.Content[_oamFifo.Position].Color != 0 && (entry.Color == 0 || !(_oamFifo.Content[_oamFifo.Position].Attributes & 0x80))) {
				entry = _oamFifo.Content[_oamFifo.Position];
				isSprite = true;
			}

			uint16_t rgbColor;
			if(_gameboy->IsCgb()) {
				if(isSprite) {
					rgbColor = _state.CgbObjPalettes[entry.Color | ((entry.Attributes & 0x07) << 2)];
				} else {
					rgbColor = _state.CgbBgPalettes[entry.Color | ((entry.Attributes & 0x07) << 2)];
				}
			} else {
				if(isSprite) {
					rgbColor = bwRgbPalette[(((entry.Attributes & 0x10) ? _state.ObjPalette1 : _state.ObjPalette0) >> (entry.Color * 2)) & 0x03];
				} else {
					rgbColor = bwRgbPalette[(_state.BgPalette >> (entry.Color * 2)) & 0x03];
				}
			}
			_currentBuffer[outOffset] = rgbColor;
		}

		_bgFifo.Pop();
		_drawnPixels++;

		if(_oamFifo.Size > 0) {
			_oamFifo.Pop();
		}
	}

	ClockTileFetcher();
}

void GbPpu::RunSpriteEvaluation()
{
	if(_state.Cycle & 0x01) {
		if(_spriteCount < 10) {
			uint8_t spriteIndex = ((_state.Cycle - 4) >> 1) * 4;
			int16_t sprY = (int16_t)_oam[spriteIndex] - 16;
			if(_state.Scanline >= sprY && _state.Scanline < sprY + (_state.LargeSprites ? 16 : 8)) {
				_spriteX[_spriteCount] = _oam[spriteIndex + 1];
				_spriteIndexes[_spriteCount] = spriteIndex;
				_spriteCount++;
			}
		}
	} else {
		//TODO check proper timing for even&odd cycles
	}
}

void GbPpu::ResetRenderer()
{	
	//Reset fetcher & pixel FIFO
	_oamFifo.Reset();
	_oamFetcher.Step = 0;

	_bgFifo.Reset();
	_bgFifo.Size = 8;
	_bgFetcher.Step = 0;

	_drawnPixels = -8 - (_state.ScrollX & 0x07);
	_fetchSprite = -1;
	_fetchWindow = false;
	_fetchColumn = _state.ScrollX / 8;
}

void GbPpu::ClockSpriteFetcher()
{
	switch(_oamFetcher.Step++) {
		case 1: {
			//Fetch tile index
			int16_t sprY = (int16_t)_oam[_fetchSprite] - 16;
			uint8_t sprTile = _oam[_fetchSprite + 2];
			uint8_t sprAttr = _oam[_fetchSprite + 3];
			bool vMirror = (sprAttr & 0x40) != 0;
			uint16_t tileBank = _gameboy->IsCgb() ? ((sprAttr & 0x08) ? 0x2000 : 0x0000) : 0;

			uint8_t sprOffsetY = vMirror ? (_state.LargeSprites ? 15 : 7) - (_state.Scanline - sprY) : (_state.Scanline - sprY);
			if(_state.LargeSprites) {
				sprTile &= 0xFE;
			}

			uint16_t sprTileAddr = (sprTile * 16 + sprOffsetY * 2) | tileBank;
			_oamFetcher.Addr = sprTileAddr;
			_oamFetcher.Attributes = sprAttr;
			break;
		}

		case 3: _oamFetcher.LowByte = _vram[_oamFetcher.Addr]; break;

		case 5: {
			//Fetch sprite data (high byte)
			_oamFetcher.HighByte = _vram[_oamFetcher.Addr + 1];
			PushSpriteToPixelFifo();
			break;
		}
	}
}

void GbPpu::FindNextSprite()
{
	if(_prevSprite < _spriteCount && _fetchSprite < 0 && (_state.SpritesEnabled || _gameboy->IsCgb())) {
		for(int i = _prevSprite; i < _spriteCount; i++) {
			if((int)_spriteX[i] - 8 == _drawnPixels) {
				_fetchSprite = _spriteIndexes[i];
				_prevSprite++;
				_oamFetcher.Step = 0;
				break;
			}
		}
	}
}

void GbPpu::ClockTileFetcher()
{
	FindNextSprite();
	if(_fetchSprite >= 0 && _bgFetcher.Step >= 5 && _bgFifo.Size > 0) {
		_evtColor = EvtColor::RenderingOamLoad;
		ClockSpriteFetcher();
		FindNextSprite();
		return;
	}

	_evtColor = EvtColor::RenderingBgLoad;

	switch(_bgFetcher.Step++) {
		case 1: {
			//Fetch tile index
			uint16_t tilemapAddr;
			uint8_t yOffset;
			if(_fetchWindow) {
				tilemapAddr = _state.WindowTilemapSelect ? 0x1C00 : 0x1800;
				yOffset = _state.Scanline - _state.WindowY;
			} else {
				tilemapAddr = _state.BgTilemapSelect ? 0x1C00 : 0x1800;
				yOffset = _state.ScrollY + _state.Scanline;
			}

			uint8_t row = yOffset >> 3;
			uint16_t tileAddr = tilemapAddr + _fetchColumn + row * 32;
			uint8_t tileIndex = _vram[tileAddr];

			uint8_t attributes = _gameboy->IsCgb() ? _vram[tileAddr | 0x2000] : 0;
			bool vMirror = (attributes & 0x40) != 0;
			uint16_t tileBank = (attributes & 0x08) ? 0x2000 : 0x0000;

			uint16_t baseTile = _state.BgTileSelect ? 0 : 0x1000;
			uint8_t tileY = vMirror ? (7 - (yOffset & 0x07)) : (yOffset & 0x07);
			uint16_t tileRowAddr = baseTile + (baseTile ? (int8_t)tileIndex * 16 : tileIndex * 16) + tileY * 2;
			tileRowAddr |= tileBank;
			_bgFetcher.Addr = tileRowAddr;
			_bgFetcher.Attributes = (attributes & 0xBF);
			break;
		}

		case 3: {
			//Fetch tile data (low byte)
			_bgFetcher.LowByte = _vram[_bgFetcher.Addr];
			break;
		}

		case 5: {
			//Fetch tile data (high byte)
			_bgFetcher.HighByte = _vram[_bgFetcher.Addr + 1];
			
			//Fallthrough
		}

		case 6:
		case 7:
			if(_bgFifo.Size == 0) {
				PushTileToPixelFifo();
			} else if(_bgFetcher.Step == 8) {
				//Wait until fifo is empty to push pixels
				_bgFetcher.Step = 7;
			}
			break;
	}
}

void GbPpu::PushSpriteToPixelFifo()
{
	_fetchSprite = -1;
	_oamFetcher.Step = 0;

	if(!_state.SpritesEnabled) {
		return;
	}

	uint8_t pos = _oamFifo.Position;

	//Overlap sprite
	for(int i = 0; i < 8; i++) {
		uint8_t shift = (_oamFetcher.Attributes & 0x20) ? i : (7 - i);
		uint8_t bits = ((_oamFetcher.LowByte >> shift) & 0x01);
		bits |= ((_oamFetcher.HighByte >> shift) & 0x01) << 1;

		if(bits > 0 && _oamFifo.Content[pos].Color == 0) {
			_oamFifo.Content[pos].Color = bits;
			_oamFifo.Content[pos].Attributes = _oamFetcher.Attributes;
		}
		pos = (pos + 1) & 0x07;
	}
	_oamFifo.Size = 8;
}

void GbPpu::PushTileToPixelFifo()
{
	//Add new tile to fifo
	for(int i = 0; i < 8; i++) {
		uint8_t shift = (_bgFetcher.Attributes & 0x20) ? i : (7 - i);
		uint8_t bits = ((_bgFetcher.LowByte >> shift) & 0x01);
		bits |= ((_bgFetcher.HighByte >> shift) & 0x01) << 1;

		_bgFifo.Content[i].Color = _state.BgEnabled ? bits : 0;
		_bgFifo.Content[i].Attributes = _bgFetcher.Attributes;
	}

	_fetchColumn = (_fetchColumn + 1) & 0x1F;
	_bgFifo.Position = 0;
	_bgFifo.Size = 8;
	_bgFetcher.Step = 0;
}

void GbPpu::ChangeMode(PpuMode mode)
{
	_state.Mode = mode;
}

void GbPpu::UpdateLyCoincidenceFlag()
{
	if(_state.Scanline < 153) {
		_state.LyCoincidenceFlag = (_state.LyCompare == _state.Scanline) && (_state.Cycle >= 4 || _state.Scanline == 0);
	} else {
		if(_state.LyCompare == 153) {
			_state.LyCoincidenceFlag = (_state.LyCompare == _state.Scanline) && _state.Cycle >= 4 && _state.Cycle < 8;
		} else {
			_state.LyCoincidenceFlag = (_state.LyCompare == 0) && _state.Cycle >= 12;
		}
	}
}

void GbPpu::UpdateStatIrq()
{
	bool irqFlag = (
		_state.LcdEnabled &&
		((_state.LyCoincidenceFlag && (_state.Status & GbPpuStatusFlags::CoincidenceIrq)) ||
		(_state.Mode == PpuMode::HBlank && (_state.Status & GbPpuStatusFlags::HBlankIrq)) ||
		(_state.Mode == PpuMode::OamEvaluation && (_state.Status & GbPpuStatusFlags::OamIrq)) ||
		(_state.Mode == PpuMode::VBlank && ((_state.Status & GbPpuStatusFlags::VBlankIrq) || (_state.Status & GbPpuStatusFlags::OamIrq))))
	);

	if(irqFlag && !_state.StatIrqFlag) {
		_memoryManager->RequestIrq(GbIrqSource::LcdStat);
	}
	_state.StatIrqFlag = irqFlag;
}

void GbPpu::GetPalette(uint16_t out[4], uint8_t palCfg)
{
	out[0] = bwRgbPalette[palCfg & 0x03];
	out[1] = bwRgbPalette[(palCfg >> 2) & 0x03];
	out[2] = bwRgbPalette[(palCfg >> 4) & 0x03];
	out[3] = bwRgbPalette[(palCfg >> 6) & 0x03];
}

void GbPpu::SendFrame()
{
	_console->ProcessEvent(EventType::EndFrame);
	_state.FrameCount++;
	_console->GetNotificationManager()->SendNotification(ConsoleNotificationType::PpuFrameDone);

#ifdef LIBRETRO
	_console->GetVideoDecoder()->UpdateFrameSync(_currentBuffer, 256, 239, _state.FrameCount, false);
#else
	if(_console->GetRewindManager()->IsRewinding()) {
		_console->GetVideoDecoder()->UpdateFrameSync(_currentBuffer, 256, 239, _state.FrameCount, true);
	} else {
		_console->GetVideoDecoder()->UpdateFrame(_currentBuffer, 256, 239, _state.FrameCount);
	}
#endif

	//TODO move this somewhere that makes more sense
	uint8_t prevInput = _memoryManager->ReadInputPort();
	_console->ProcessEndOfFrame();
	uint8_t newInput = _memoryManager->ReadInputPort();
	if(prevInput != newInput) {
		_memoryManager->RequestIrq(GbIrqSource::Joypad);
	}

	_currentBuffer = _currentBuffer == _outputBuffers[0] ? _outputBuffers[1] : _outputBuffers[0];
}

uint8_t GbPpu::Read(uint16_t addr)
{
	switch(addr) {
		case 0xFF40: return _state.Control;
		case 0xFF41:
			//FF41 - STAT - LCDC Status (R/W)
			return (
				0x80 | 
				(_state.Status & 0x78) |
				(_state.LyCoincidenceFlag ? 0x04 : 0x00) |
				(int)_state.Mode
			);

		case 0xFF42: return _state.ScrollY; //FF42 - SCY - Scroll Y (R/W)
		case 0xFF43: return _state.ScrollX; //FF43 - SCX - Scroll X (R/W)
		case 0xFF44: return _state.Scanline; //FF44 - LY - LCDC Y-Coordinate (R)
		case 0xFF45: return _state.LyCompare; //FF45 - LYC - LY Compare (R/W)
		case 0xFF47: return _state.BgPalette; //FF47 - BGP - BG Palette Data (R/W) - Non CGB Mode Only
		case 0xFF48: return _state.ObjPalette0; //FF48 - OBP0 - Object Palette 0 Data (R/W) - Non CGB Mode Only
		case 0xFF49: return _state.ObjPalette1; //FF49 - OBP1 - Object Palette 1 Data (R/W) - Non CGB Mode Only
		case 0xFF4A: return _state.WindowY; //FF4A - WY - Window Y Position (R/W)
		case 0xFF4B: return _state.WindowX; //FF4B - WX - Window X Position minus 7 (R/W)
	}
	
	LogDebug("[Debug] GB - Missing read handler: $" + HexUtilities::ToHex(addr));
	return 0xFF;
}

void GbPpu::Write(uint16_t addr, uint8_t value)
{
	switch(addr) {
		case 0xFF40: 
			_state.Control = value; 
			if(_state.LcdEnabled != ((value & 0x80) != 0)) {
				_state.LcdEnabled = (value & 0x80) != 0;
				
				if(!_state.LcdEnabled) {
					//Reset LCD to top of screen when it gets turned off
					_state.Cycle = 0;
					_state.Scanline = 0;
					ChangeMode(PpuMode::HBlank);

					//Send a blank (white) frame
					_lastFrameTime = _gameboy->GetCycleCount();
					std::fill(_outputBuffers[0], _outputBuffers[0] + 256 * 239, 0x7FFF);
					std::fill(_outputBuffers[1], _outputBuffers[1] + 256 * 239, 0x7FFF);
					SendFrame();
				} else {
					_state.Cycle = 4;
					_state.Scanline = 0;
					ResetRenderer();
					ChangeMode(PpuMode::HBlank);
					UpdateLyCoincidenceFlag();
					UpdateStatIrq();
					
					_console->ProcessEvent(EventType::StartFrame);
					if(_console->IsDebugging()) {
						_currentEventViewerBuffer = _currentEventViewerBuffer == _eventViewerBuffers[0] ? _eventViewerBuffers[1] : _eventViewerBuffers[0];
						for(int i = 0; i < 456 * 154; i++) {
							_currentEventViewerBuffer[i] = 0x18C6;
						}
					}
				}
			}
			_state.WindowTilemapSelect = (value & 0x40) != 0;
			_state.WindowEnabled = (value & 0x20) != 0;
			_state.BgTileSelect = (value & 0x10) != 0;
			_state.BgTilemapSelect = (value & 0x08) != 0;
			_state.LargeSprites = (value & 0x04) != 0;
			_state.SpritesEnabled = (value & 0x02) != 0;
			_state.BgEnabled = (value & 0x01) != 0;
			break;

		case 0xFF41:
			_state.Status = value & 0xF8; 
			UpdateStatIrq();
			break;

		case 0xFF42: _state.ScrollY = value; break;
		case 0xFF43: _state.ScrollX = value; break;
		case 0xFF45: _state.LyCompare = value; break;
		case 0xFF47: _state.BgPalette = value; break;
		case 0xFF48: _state.ObjPalette0 = value; break;
		case 0xFF49: _state.ObjPalette1 = value; break;
		case 0xFF4A: _state.WindowY = value; break;
		case 0xFF4B: _state.WindowX = value; break;

		default:
			LogDebug("[Debug] GB - Missing write handler: $" + HexUtilities::ToHex(addr));
			break;
	}
}

uint8_t GbPpu::ReadVram(uint16_t addr)
{
	if((int)_state.Mode <= (int)PpuMode::OamEvaluation) {
		return _vram[(_state.CgbVramBank << 13) | (addr & 0x1FFF)];
	} else {
		return 0xFF;
	}
}

void GbPpu::WriteVram(uint16_t addr, uint8_t value)
{
	if((int)_state.Mode <= (int)PpuMode::OamEvaluation) {
		_vram[(_state.CgbVramBank << 13) | (addr & 0x1FFF)] = value;
	}
}

uint8_t GbPpu::ReadOam(uint8_t addr)
{
	if(addr < 0xA0) {
		if((int)_state.Mode >= (int)PpuMode::OamEvaluation || _memoryManager->IsOamDmaRunning()) {
			return 0xFF;
		} else {
			return _oam[addr];
		}
	}
	return 0;
}

void GbPpu::WriteOam(uint8_t addr, uint8_t value, bool forDma)
{
	//During DMA or rendering/oam evaluation, ignore writes to OAM
	//The DMA controller is always allowed to write to OAM (presumably the PPU can't read OAM during that time? TODO implement)
	//On the DMG, there is apparently a ~4 clock gap (80 to 84) between OAM evaluation & rendering where writing is allowed?
	if(addr < 0xA0) {
		if(forDma || ((int)_state.Mode <= (int)PpuMode::VBlank && !_memoryManager->IsOamDmaRunning()) || (_state.Cycle >= 80 && _state.Cycle < 84)) {
			_oam[addr] = value;
		}
	}
}

uint8_t GbPpu::ReadCgbRegister(uint16_t addr)
{
	switch(addr) {
		case 0xFF4F: return _state.CgbVramBank;
		case 0xFF68: return _state.CgbBgPalPosition | (_state.CgbBgPalAutoInc ? 0x80 : 0);
		case 0xFF69: return (_state.CgbBgPalettes[_state.CgbBgPalPosition >> 1] >> ((_state.CgbBgPalPosition & 0x01) ? 8 : 0) & 0xFF);
		case 0xFF6A: return _state.CgbObjPalPosition | (_state.CgbObjPalAutoInc ? 0x80 : 0);
		case 0xFF6B: return (_state.CgbObjPalettes[_state.CgbObjPalPosition >> 1] >> ((_state.CgbObjPalPosition & 0x01) ? 8 : 0) & 0xFF);
	}
	LogDebug("[Debug] GBC - Missing read handler: $" + HexUtilities::ToHex(addr));
	return 0;
}

void GbPpu::WriteCgbRegister(uint16_t addr, uint8_t value)
{
	switch(addr) {
		case 0xFF4F: _state.CgbVramBank = value & 0x01; break;
		
		case 0xFF68:
			//FF68 - BCPS/BGPI - CGB Mode Only - Background Palette Index
			_state.CgbBgPalPosition = value & 0x3F;
			_state.CgbBgPalAutoInc = (value & 0x80) != 0;
			break;

		case 0xFF69: {
			//FF69 - BCPD/BGPD - CGB Mode Only - Background Palette Data
			WriteCgbPalette(_state.CgbBgPalPosition, _state.CgbBgPalettes, _state.CgbBgPalAutoInc, value);
			break;
		}

		case 0xFF6A:
			//FF6A - OCPS/OBPI - CGB Mode Only - Sprite Palette Index
			_state.CgbObjPalPosition = value & 0x3F;
			_state.CgbObjPalAutoInc = (value & 0x80) != 0;
			break;

		case 0xFF6B:
			//FF6B - OCPD/OBPD - CGB Mode Only - Sprite Palette Data
			WriteCgbPalette(_state.CgbObjPalPosition, _state.CgbObjPalettes, _state.CgbObjPalAutoInc, value);
			break;

		default:
			LogDebug("[Debug] GBC - Missing write handler: $" + HexUtilities::ToHex(addr));
			break;
	}
}

void GbPpu::WriteCgbPalette(uint8_t& pos, uint16_t* pal, bool autoInc, uint8_t value)
{
	if((int)_state.Mode <= (int)PpuMode::OamEvaluation) {
		if(pos & 0x01) {
			pal[pos >> 1] = (pal[pos >> 1] & 0xFF) | ((value & 0x7F) << 8);
		} else {
			pal[pos >> 1] = (pal[pos >> 1] & 0xFF00) | value;
		}
	}

	if(autoInc) {
		pos = (pos + 1) & 0x3F;
	}
}

void GbPpu::Serialize(Serializer& s)
{
	s.Stream(
		_state.Scanline, _state.Cycle, _state.Mode, _state.LyCompare, _state.BgPalette, _state.ObjPalette0, _state.ObjPalette1,
		_state.ScrollX, _state.ScrollY, _state.WindowX, _state.WindowY, _state.Control, _state.LcdEnabled, _state.WindowTilemapSelect,
		_state.WindowEnabled, _state.BgTileSelect, _state.BgTilemapSelect, _state.LargeSprites, _state.SpritesEnabled, _state.BgEnabled,
		_state.Status, _state.FrameCount, _lastFrameTime,
		_state.CgbBgPalAutoInc, _state.CgbBgPalPosition,
		_state.CgbObjPalAutoInc, _state.CgbObjPalPosition, _state.CgbVramBank
	);

	s.StreamArray(_state.CgbBgPalettes, 4 * 8);
	s.StreamArray(_state.CgbObjPalettes, 4 * 8);

	s.Stream(
		_bgFetcher.Attributes, _bgFetcher.Step, _bgFetcher.Addr, _bgFetcher.LowByte, _bgFetcher.HighByte,
		_oamFetcher.Attributes, _oamFetcher.Step, _oamFetcher.Addr, _oamFetcher.LowByte, _oamFetcher.HighByte,
		_drawnPixels, _fetchColumn, _fetchWindow, _fetchSprite, _spriteCount, _prevSprite,
		_bgFifo.Position, _bgFifo.Size, _oamFifo.Position, _oamFifo.Size
	);

	for(int i = 0; i < 8; i++) {
		s.Stream(_bgFifo.Content[i].Color, _bgFifo.Content[i].Attributes);
		s.Stream(_oamFifo.Content[i].Color, _oamFifo.Content[i].Attributes);
	}

	s.StreamArray(_spriteX, 10);
	s.StreamArray(_spriteIndexes, 10);
}