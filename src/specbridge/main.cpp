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
	@brief Program entry point
 */

#include "specbridge.h"
#include "AseqSCPIServer.h"
#include <signal.h>

using namespace std;

vector<string> explode(const string& str, char separator);
string Trim(const string& str);

void help();

void help()
{
	fprintf(stderr,
			"specbridge [general options] [logger options]\n"
			"\n"
			"  [general options]:\n"
			"    --help                        : this message...\n"
			"    --scpi-port port              : specifies the SCPI control plane port (default 5025)\n"
			"    --waveform-port port          : specifies the binary waveform data port (default 5026)\n"
			"\n"
			"  [logger options]:\n"
			"    levels: ERROR, WARNING, NOTICE, VERBOSE, DEBUG\n"
			"    --quiet|-q                    : reduce logging level by one step\n"
			"    --verbose                     : set logging level to VERBOSE\n"
			"    --debug                       : set logging level to DEBUG\n"
			"    --trace <classname>|          : name of class with tracing messages. (Only relevant when logging level is DEBUG.)\n"
			"            <classname::function>\n"
			"    --logfile|-l <filename>       : output log messages to file\n"
			"    --logfile-lines|-L <filename> : output log messages to file, with line buffering\n"
			"    --stdout-only                 : writes errors/warnings to stdout instead of stderr\n"
		   );
}

string g_model;
string g_serial;
//string g_fwver;

Socket g_scpiSocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
Socket g_dataSocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);

#ifdef _WIN32
BOOL WINAPI OnQuit(DWORD signal);
#else
void OnQuit(int signal);
#endif

uintptr_t g_hDevice = 0;

int g_numPixels = 3653;

void ReadCalData();

//Wavelengths, in nm, of each spectral bin
vector<float> g_wavelengths;
vector<float> g_sensorResponse;

int main(int argc, char* argv[])
{
	//Global settings
	Severity console_verbosity = Severity::NOTICE;

	//Parse command-line arguments
	uint16_t scpi_port = 5025;
	uint16_t waveform_port = 5026;
	for(int i=1; i<argc; i++)
	{
		string s(argv[i]);

		//Let the logger eat its args first
		if(ParseLoggerArguments(i, argc, argv, console_verbosity))
			continue;

		if(s == "--help")
		{
			help();
			return 0;
		}

		else if(s == "--scpi-port")
		{
			if(i+1 < argc)
				scpi_port = atoi(argv[++i]);
		}

		else if(s == "--waveform-port")
		{
			if(i+1 < argc)
				waveform_port = atoi(argv[++i]);
		}

		else
		{
			fprintf(stderr, "Unrecognized command-line argument \"%s\", use --help\n", s.c_str());
			return 1;
		}
	}

	//Set up logging
	g_log_sinks.emplace(g_log_sinks.begin(), new ColoredSTDLogSink(console_verbosity));

	//Try to find a spectrometer
	vector<string> serials;
	auto info = getDevicesInfo();
	LogDebug("Found %u spectrometer(s)\n", getDevicesCount());
	for(DeviceInfo_t* p = info; p != nullptr; p = p->next)
	{
		LogIndenter li;
		LogDebug("S/N: %s\n", p->serialNumber);
		serials.push_back(p->serialNumber);
	}
	clearDevicesInfo(info);

	//Connect to the device
	//connectToDeviceBySerial seems broken! always outputs null...
	unsigned int ndevice = 0;
	LogDebug("Connecting to spectrometer with USB interface serial %s...\n", serials[ndevice].c_str());
	int err;
	if(0 != (err = connectToDeviceByIndex(ndevice, &g_hDevice) ))
	{
		LogError("failed to connect to device code %d\n", err);
		if(err == CONNECT_ERROR_FAILED)
			LogNotice("CONNECT_ERROR_FAILED, check permissions on /dev/hidrawX file\n");
		return 1;
	}
	LogNotice("Successfully opened instrument\n");

	ReadCalData();

	//Set initial frame format
	//Frame contains 32 dummy pixels, valid data, 14 dummy pixels
	uint16_t framesize;
	if(0 != (err = setFrameFormat(0, g_numPixels-1, 0, &framesize, &g_hDevice)))
	{
		LogError("failed to set frame format, code %d\n", err);
		return 1;
	}
	//LogDebug("framesize = %d\n", framesize);

	//Set exposure, in 10us units
	//Do 125ms
	auto exposure = 12500;
	if(0 != (err = setExposure(exposure, 0, &g_hDevice)))
	{
		LogError("failed to set exposure, code %d\n", err);
		return 1;
	}

	//Set acquisition parameters to free run capture with no averaging
	if(0 != (err = setAcquisitionParameters(1, 0, 0, exposure, &g_hDevice)))
	{
		LogError("failed to set acquisition parameters, code %d\n", err);
		return 1;
	}

	//Do not use external trigger
	if(0 != (err = setExternalTrigger(0, 0, &g_hDevice)))
	{
		LogError("failed to set trigger mode, code %d\n", err);
		return 1;
	}

	//Start capturing
	if(0 != (err = triggerAcquisition(&g_hDevice)))
	{
		LogError("failed to trigger acquisition, code %d\n", err);
		return 1;
	}

	//get the frame data
	uint16_t* framePixels = new uint16_t[framesize];
	if(0 != (err = getFrame(framePixels, 0xffff, &g_hDevice)))
	{
		LogError("failed to get frame, code %d\n", err);
		return 1;
	}

	//frame data seems to be *mirrored*? shortest wavelengths at right

	//Dump frame data
	/*LogDebug("wavelength,counts\n");
	for(int i=g_numPixels-1; i>=0; i--)
		LogDebug("%.0f,%d\n", g_wavelengths[i]*1000, (int)framePixels[i+32]);*/

	//Clean up
	delete[] framePixels;

	//Set up signal handlers
#ifdef _WIN32
	SetConsoleCtrlHandler(OnQuit, TRUE);
#else
	signal(SIGINT, OnQuit);
	signal(SIGPIPE, SIG_IGN);
#endif

	//Configure the data plane socket
	g_dataSocket.Bind(waveform_port);
	g_dataSocket.Listen();

	//Launch the control plane socket server
	g_scpiSocket.Bind(scpi_port);
	g_scpiSocket.Listen();

	LogDebug("Ready\n");

	while(true)
	{
		Socket scpiClient = g_scpiSocket.Accept();
		if(!scpiClient.IsValid())
			break;

		//Create a server object for this connection
		AseqSCPIServer server(scpiClient.Detach());

		//Launch the data-plane thread
		thread dataThread(WaveformServerThread);

		//Process connections on the socket
		server.MainLoop();

		g_waveformThreadQuit = true;
		dataThread.join();
		g_waveformThreadQuit = false;
	}

	OnQuit(SIGQUIT);
	return 0;
}

#ifdef _WIN32
BOOL WINAPI OnQuit(DWORD signal)
{
	(void)signal;
#else
void OnQuit(int /*signal*/)
{
#endif
	LogNotice("Shutting down...\n");

	disconnectDeviceContext(&g_hDevice);

	exit(0);
}

void ReadCalData()
{
	//Read calibration data
	LogDebug("Reading calibration data...\n");
	LogIndenter li;
	const int ncal = 97264;	//TODO: is this always the same size?
	char buf[ncal+1];
	int err;
	if(0 != (err = readFlash((uint8_t*)buf, 0, ncal, &g_hDevice)))
	{
		LogError("failed to read cal data, code %d\n", err);
		exit(1);
	}
	buf[ncal] = '\0';

	//Parse the text into lines
	string sbuf(buf);
	auto lines = explode(sbuf, '\n');
	LogDebug("Found %zu lines of data\n", lines.size());

	//First line: model c.[Y|N] serial
	auto firstFields = explode(lines[0], ' ');
	g_model = Trim(firstFields[0]);
	g_serial = Trim(firstFields[2]);
	LogDebug("Spectrometer is model %s, serial %s\n", g_model.c_str(), g_serial.c_str());
	bool hasAbsCal = (firstFields[1] == "c.Y");
	if(hasAbsCal)
		LogDebug("Absolute cal data present\n");

	//Starting at line 13 (one based, per docs) of the file we have 3653 spectral bins worth of wavelength data
	for(int i=0; i<g_numPixels; i++)
		g_wavelengths.push_back(atof(lines[i+12].c_str()));
	LogDebug("First pixel is %.3f nm\n", g_wavelengths[0]);
	LogDebug("Last pixel is %.3f nm\n", g_wavelengths[g_numPixels-1]);

	//Skip a blank line

	//Read the sensor response normalization data
	for(int i=0; i<g_numPixels; i++)
		g_sensorResponse.push_back(atof(lines[i+13+g_numPixels].c_str()));
	LogDebug("First pixel norm coeff is %.3f\n", g_sensorResponse[0]);
	LogDebug("Mid pixel norm coeff is %.3f\n", g_sensorResponse[2365]);
	LogDebug("Last pixel norm coeff is %.3f\n", g_sensorResponse[g_numPixels-1]);

	//TODO: read absolute irradiance data, if present
}

/**
	@brief Splits a string up into an array separated by delimiters
 */
vector<string> explode(const string& str, char separator)
{
	vector<string> ret;
	string tmp;
	for(auto c : str)
	{
		if(c == separator)
		{
			if(!tmp.empty())
				ret.push_back(tmp);
			tmp = "";
		}
		else
			tmp += c;
	}
	if(!tmp.empty())
		ret.push_back(tmp);
	return ret;
}

/**
	@brief Removes whitespace from the start and end of a string
 */
string Trim(const string& str)
{
	string ret;
	string tmp;

	//Skip leading spaces
	size_t i=0;
	for(; i<str.length() && isspace(str[i]); i++)
	{}

	//Read non-space stuff
	for(; i<str.length(); i++)
	{
		//Non-space
		char c = str[i];
		if(!isspace(c))
		{
			ret = ret + tmp + c;
			tmp = "";
		}

		//Space. Save it, only append if we have non-space after
		else
			tmp += c;
	}

	return ret;
}
