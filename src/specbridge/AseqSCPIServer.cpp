/***********************************************************************************************************************
*                                                                                                                      *
* specbridge                                                                                                           *
*                                                                                                                      *
* Copyright (c) 2012-2023 Andrew D. Zonenberg                                                                          *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@author Andrew D. Zonenberg
	@brief SCPI server. Control plane traffic only, no waveform data.

	SCPI commands supported:

		*IDN?
			Returns a standard SCPI instrument identification string

		POINTS?
			Returns the number of pixels in the spectrometer

		WAVELENGTHS?
			Returns a list of wavelengths for each spectral bin

		FLATCAL?
			Returns flatness correction data (block 2 of cal file)

		IRRCOEFF?
			Returns irradiance correction coefficient (line 2 of cal file)

		IRRCAL?
			Returns irradiance correction data (block 3 of cal file)
 */

#include "specbridge.h"
#include "AseqSCPIServer.h"
#include <string.h>
#include <math.h>

#define __USE_MINGW_ANSI_STDIO 1 // Required for MSYS2 mingw64 to support format "%z" ...

using namespace std;

mutex g_mutex;

bool g_triggerOneShot = false;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

AseqSCPIServer::AseqSCPIServer(ZSOCKET sock)
	: BridgeSCPIServer(sock)
{
}

AseqSCPIServer::~AseqSCPIServer()
{
	LogVerbose("Client disconnected\n");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Command parsing

bool AseqSCPIServer::OnQuery(
	const string& line,
	const string& subject,
	const string& cmd)
{
	if(BridgeSCPIServer::OnQuery(line, subject, cmd))
		return true;
	else if(cmd == "POINTS")
		SendReply(to_string(g_numPixels));
	else if(cmd == "WAVELENGTHS")
	{
		string wavelengths;
		char tmp[128];
		for(int i=0; i<g_numPixels; i++)
		{
			snprintf(tmp, sizeof(tmp), "%.3f,", g_wavelengths[i]);
			wavelengths += tmp;
		}
		SendReply(wavelengths);
	}
	else if(cmd == "FLATCAL")
	{
		string flatcal;
		char tmp[128];
		for(int i=0; i<g_numPixels; i++)
		{
			snprintf(tmp, sizeof(tmp), "%.3f,", g_sensorResponse[i]);
			flatcal += tmp;
		}
		SendReply(flatcal);
	}
	else if(cmd == "IRRCOEFF")
		SendReply(to_string(g_absCal));
	else if(cmd == "IRRCAL")
	{
		string irrcal;
		char tmp[128];
		for(int i=0; i<g_numPixels; i++)
		{
			snprintf(tmp, sizeof(tmp), "%.3f,", g_absResponse[i]);
			irrcal += tmp;
		}
		SendReply(irrcal);
	}
	else
	{
		LogDebug("Unrecognized query received: %s\n", line.c_str());
	}
	return false;
}

string AseqSCPIServer::GetMake()
{
	return "ASEQ Instruments";
}

string AseqSCPIServer::GetModel()
{
	return g_model;
}

string AseqSCPIServer::GetSerial()
{
	return g_serial;
}

string AseqSCPIServer::GetFirmwareVersion()
{
	return "1.0";
}

size_t AseqSCPIServer::GetAnalogChannelCount()
{
	return 1;
}

vector<size_t> AseqSCPIServer::GetSampleRates()
{
	vector<size_t> rates;
	rates.push_back(1);
	return rates;
}

vector<size_t> AseqSCPIServer::GetSampleDepths()
{
	vector<size_t> depths;
	depths.push_back(g_numPixels);
	return depths;
}

bool AseqSCPIServer::OnCommand(
	const string& line,
	const string& subject,
	const string& cmd,
	const vector<string>& args)
{
	if(BridgeSCPIServer::OnCommand(line, subject, cmd, args))
		return true;
	else if(cmd == "EXPOSURE")
	{
		lock_guard<mutex> lock(g_mutex);

		//convert fs to 10us ticks so e-10
		int err;
		double exposure = stod(args[0]) * 1e-10;
		if(0 != (err = setExposure(exposure, 0, &g_hDevice)))
			LogError("failed to set exposure, code %d\n", err);
	}
	else
		LogError("Unrecognized command %s\n", line.c_str());

	return true;
}

bool AseqSCPIServer::GetChannelID(const string& /*subject*/, size_t& id_out)
{
	id_out = 0;
	return true;
}

BridgeSCPIServer::ChannelType AseqSCPIServer::GetChannelType(size_t /*channel*/)
{
	return CH_ANALOG;
}

void AseqSCPIServer::AcquisitionStart(bool oneShot)
{
	g_triggerArmed = true;
	g_triggerOneShot = oneShot;
}

void AseqSCPIServer::AcquisitionForceTrigger()
{
	g_triggerArmed = true;
}

void AseqSCPIServer::AcquisitionStop()
{
	g_triggerArmed = false;
}

void AseqSCPIServer::SetChannelEnabled(size_t /*chIndex*/, bool /*enabled*/)
{
}

void AseqSCPIServer::SetAnalogCoupling(size_t /*chIndex*/, const std::string& /*coupling*/)
{
}

void AseqSCPIServer::SetAnalogRange(size_t /*chIndex*/, double /*range_V*/)
{
}

void AseqSCPIServer::SetAnalogOffset(size_t /*chIndex*/, double /*offset_V*/)
{
}

void AseqSCPIServer::SetDigitalThreshold(size_t /*chIndex*/, double /*threshold_V*/)
{
}

void AseqSCPIServer::SetDigitalHysteresis(size_t /*chIndex*/, double /*hysteresis*/)
{
}

void AseqSCPIServer::SetSampleRate(uint64_t /*rate_hz*/)
{
}

void AseqSCPIServer::SetSampleDepth(uint64_t /*depth*/)
{
}

void AseqSCPIServer::SetTriggerDelay(uint64_t /*delay_fs*/)
{
}

void AseqSCPIServer::SetTriggerSource(size_t /*chIndex*/)
{
}

void AseqSCPIServer::SetTriggerLevel(double /*level_V*/)
{
}

void AseqSCPIServer::SetTriggerTypeEdge()
{
	//all triggers are edge, nothing to do here until we start supporting other trigger types
}

bool AseqSCPIServer::IsTriggerArmed()
{
	//return g_triggerArmed;
	return true;
}

void AseqSCPIServer::SetEdgeTriggerEdge(const string& /*edge*/)
{

}
