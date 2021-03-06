//
// Copyright (C) 2013-2018 University of Amsterdam
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "engine.h"

#include <sstream>
#include <cstdio>

#include "../JASP-Common/analysisloader.h"
#include "../JASP-Common/tempfiles.h"
#include "../JASP-Common/utils.h"
#include "../JASP-Common/sharedmemory.h"

#include "rbridge.h"



#ifdef __WIN32__

#undef Realloc
#undef Free

#endif

using namespace std;
using namespace boost::interprocess;
using namespace boost::posix_time;

Engine::Engine()
{

	
	tempfiles_attach(ProcessInfo::parentPID());

	rbridge_setDataSetSource(boost::bind(&Engine::provideDataSet, this));
	rbridge_setFileNameSource(boost::bind(&Engine::provideTempFileName, this, _1, _2, _3));
	rbridge_setStateFileSource(boost::bind(&Engine::provideStateFileName, this, _1, _2));
	
	rbridge_init();
}

void Engine::setSlaveNo(int no)
{
	_slaveNo = no;
}

void Engine::saveImage()
{
	if (_status != saveImg)
		return;

	vector<string> tempFilesFromLastTime = tempfiles_retrieveList(_analysisId);

	std::string name = _imageOptions.get("name", Json::nullValue).asString();
	std::string type = _imageOptions.get("type", Json::nullValue).asString();

	int height = _imageOptions.get("height", Json::nullValue).asInt();
	int width = _imageOptions.get("width", Json::nullValue).asInt();
	std::string result = rbridge_saveImage(name, type, height, width, _ppi);


	_status = complete;
	Json::Reader parser;
	parser.parse(result, _analysisResults, false);
	_analysisResults["results"]["inputOptions"] = _imageOptions;
	_progress = -1;
	sendResults();
	_status = empty;

}

void Engine::editImage()
{
    if (_status != editImg)
        return;

    vector<string> tempFilesFromLastTime = tempfiles_retrieveList(_analysisId);

    RCallback callback = boost::bind(&Engine::callback, this, _1, _2);

    std::string name = _imageOptions.get("name", Json::nullValue).asString();
		std::string type = _imageOptions.get("type", Json::nullValue).asString();
    int height = _imageOptions.get("height", Json::nullValue).asInt();
    int width = _imageOptions.get("width", Json::nullValue).asInt();
    std::string result = rbridge_editImage(name, type, height, width, _ppi);

    _status = complete;
    Json::Reader parser;
    parser.parse(result, _analysisResults, false);
    _progress = -1;
    sendResults();
    _status = empty;

    //tempfiles_deleteList(tempFilesFromLastTime);

}

void Engine::runAnalysis()
{
	if (_status == empty || _status == aborted)
		return;

	string perform;

	if (_status == toInit)
	{
		perform = "init";
		_status = initing;
	}
	else
	{
		perform = "run";
		_status = running;
	}

	vector<string> tempFilesFromLastTime = tempfiles_retrieveList(_analysisId);

	RCallback callback = boost::bind(&Engine::callback, this, _1, _2);

	_currentAnalysisKnowsAboutChange = false;
	_analysisResultsString = rbridge_run(_analysisName, _analysisTitle, _analysisRequiresInit, _analysisDataKey, _analysisOptions, _analysisResultsMeta, _analysisStateKey, perform, _ppi, callback);

	if (_status == initing || _status == running)  // if status hasn't changed
		receiveMessages();

	if (_status == toInit || _status == aborted || _status == error || _status == exception)
	{
		// analysis was aborted, and we shouldn't send the results
	}
	else if (_status == changed && (_currentAnalysisKnowsAboutChange == false || _analysisResultsString == "null"))
	{
		// analysis was changed, and the analysis either did not know about
		// the change (because it did not call a callback),
		// or it could not incorporate the changes (returned null).
		// in both cases it needs to be re-run, and results should
		// not be sent

		_status = toInit;
		if (_analysisResultsString == "null")
			tempfiles_deleteList(tempFilesFromLastTime);
			
	}
	else
	{
		Json::Reader parser;
		parser.parse(_analysisResultsString, _analysisResults, false);

		_status = _status == initing ? inited : complete;
		_progress = -1;
		sendResults();
		_status = empty;

		vector<string> filesToKeep;

		if (_analysisResults.isObject())
		{
			Json::Value filesToKeepValue = _analysisResults.get("keep", Json::nullValue);

			if (filesToKeepValue.isArray())
			{
				for (size_t i = 0; i < filesToKeepValue.size(); i++)
				{
					Json::Value fileToKeepValue = filesToKeepValue.get(i, Json::nullValue);
					if ( ! fileToKeepValue.isString())
						continue;

					filesToKeep.push_back(fileToKeepValue.asString());
				}
			}
			else if (filesToKeepValue.isString())
			{
				filesToKeep.push_back(filesToKeepValue.asString());
			}
		}

		Utils::remove(tempFilesFromLastTime, filesToKeep);

		tempfiles_deleteList(tempFilesFromLastTime);
	}
}

void Engine::run()
{
#if defined(QT_DEBUG) || defined(__linux__)
	if (_slaveNo == 0)
	{
		string engineInfo = rbridge_check();

		Json::Value v;
		Json::Reader r;
		r.parse(engineInfo, v);

		std::cout << v.toStyledString() << "\n";
		std::cout.flush();
	}
#endif

	stringstream ss;
	ss << "JASP-IPC-" << ProcessInfo::parentPID();
	string memoryName = ss.str();

	_channel = new IPCChannel(memoryName, _slaveNo, true);

	while (ProcessInfo::isParentRunning())
	{
		receiveMessages(100);
		if (_status == saveImg)
			saveImage();
        else if (_status == editImg)
            editImage();
		else
			runAnalysis();

		if(filterChanged)
			applyFilter();
	}

	shared_memory_object::remove(memoryName.c_str());
}

bool Engine::receiveMessages(int timeout)
{
	string data;

	if (_channel->receive(data, timeout))
	{
#ifdef JASP_DEBUG
		std::cout << "received message" << std::endl;
		std::cout << data << std::endl;
		std::cout.flush();
#endif
		Json::Value jsonRequest;
		Json::Reader r;
		r.parse(data, jsonRequest, false);

		if(jsonRequest.get("filter", "").asString() != "")
		{
			filterChanged = true;
			filter = jsonRequest.get("filter", "").asString();
			generatedFilter = jsonRequest.get("generatedFilter", "").asString();

			return false; //This is not an analysis-run-request or anything like that, so quit like a not-message.
		}

		int analysisId = jsonRequest.get("id", -1).asInt();
		string perform = jsonRequest.get("perform", "run").asString();

		if (analysisId == _analysisId && _status == running)
		{
			// if the current running analysis has changed

			if (perform == "init")
				_status = changed;
			else if (perform == "stop")
				_status = stopped;
			else
				_status = aborted;
		}
		else
		{
			// the new analysis should be init or run (existing analyses will be aborted)

			_analysisId = analysisId;

			if (perform == "init")
				_status = toInit;
			else if (perform == "run")
				_status = toRun;
			else if (perform == "saveImg")
				_status = saveImg;
            else if (perform == "editImg")
                _status = editImg;
			else
				_status = error;
		}

		if (_status == toInit || _status == toRun || _status == changed || _status == saveImg || _status == editImg)
		{
			_analysisName			= jsonRequest.get("name",			Json::nullValue).asString();
			_analysisTitle			= jsonRequest.get("title",			Json::nullValue).asString();
			_analysisDataKey		= jsonRequest.get("dataKey",		Json::nullValue).toStyledString();
			_analysisOptions		= jsonRequest.get("options",		Json::nullValue).toStyledString();
			_analysisResultsMeta	= jsonRequest.get("resultsMeta",	Json::nullValue).toStyledString();
			_analysisStateKey		= jsonRequest.get("stateKey",		Json::nullValue).toStyledString();
			_analysisRevision		= jsonRequest.get("revision",		-1).asInt();
			_imageOptions			= jsonRequest.get("image",			Json::nullValue);

			Json::Value analysisRequiresInit = jsonRequest.get("requiresInit", Json::nullValue);
			_analysisRequiresInit = analysisRequiresInit.isNull() ? true : analysisRequiresInit.asBool();


			Json::Value ppi, settings = jsonRequest.get("settings", Json::nullValue);
			if (settings.isObject() && (ppi = settings.get("ppi", Json::nullValue)).isInt())
				_ppi = ppi.asInt();
			else
				_ppi = 96;
		}

		return true;
	}

	return false;
}

void Engine::sendResults()
{
	Json::Value response = Json::Value(Json::objectValue);

	response["id"] = _analysisId;
	response["name"] = _analysisName;
	response["revision"] = _analysisRevision;
	response["progress"] = _progress;

	Json::Value resultsStatus = Json::nullValue;

	if (_analysisResults.isObject())
		resultsStatus = _analysisResults.get("status", Json::nullValue);

	if (resultsStatus != Json::nullValue)
	{
		response["results"] = _analysisResults.get("results", Json::nullValue);
		response["status"]  = resultsStatus.asString();
	}
	else
	{
		string status;

		switch (_status)
		{
		case inited:
			status = "inited";
			break;
		case running:
		case changed:
			status = "running";
			break;
		case complete:
			status = "complete";
			break;
		case stopped:
			status = "stopped";
			break;
		default:
			status = "error";
			break;
		}

		response["results"] = _analysisResults;
		response["status"] = status;
	}

	string message = response.toStyledString();

	_channel->send(message);
}

void Engine::sendFilterResult(std::vector<bool> filterResult)
{
	Json::Value filterResponse = Json::Value(Json::objectValue);

	Json::Value filterResultList = Json::Value(Json::arrayValue);
	for(bool f : filterResult)
		filterResultList.append(f);
	filterResponse["filterResult"] = filterResultList;

	std::string msg = filterResponse.toStyledString();
	_channel->send(msg);
}

void Engine::sendFilterError(std::string errorMessage)
{
	Json::Value filterResponse = Json::Value(Json::objectValue);

	filterResponse["filterError"] = errorMessage;

	std::string msg = filterResponse.toStyledString();
	_channel->send(msg);
}

string Engine::callback(const string &results, int progress)
{
	receiveMessages();

	if (_status == aborted || _status == toInit || _status == toRun)
		return "{ \"status\" : \"aborted\" }"; // abort

	if (_status == changed && _currentAnalysisKnowsAboutChange)
	{
		_status = running;
		_currentAnalysisKnowsAboutChange = false;
	}

	if (results != "null")
	{
		_analysisResultsString = results;

		Json::Reader parser;
		parser.parse(_analysisResultsString, _analysisResults, false);

		_progress = progress;

		sendResults();
	}
	else if (progress >= 0 && _status == running)
	{
		_analysisResultsString = Json::nullValue;
		_analysisResults = "";
		_progress = progress;

		sendResults();
		
	}

	if (_status == changed)
	{
		_currentAnalysisKnowsAboutChange = true; // because we're telling it now
		return "{ \"status\" : \"changed\", \"options\" : " + _analysisOptions + " }";
	}
	else if (_status == stopped)
	{
		return "{ \"status\" : \"stopped\" }";
	}
	else if (_status == aborted)
	{
		return "{ \"status\" : \"aborted\" }";
	}

	return "{ \"status\" : \"ok\" }";
}

DataSet * Engine::provideDataSet()
{
	return SharedMemory::retrieveDataSet();
}

void Engine::provideStateFileName(string &root, string &relativePath)
{
	return tempfiles_createSpecific("state", _analysisId, root, relativePath);
}

void Engine::provideTempFileName(const string &extension, string &root, string &relativePath)
{	
	tempfiles_create(extension, _analysisId, root, relativePath);
}

void Engine::applyFilter()
{
	filterChanged = false;

	try
	{
		std::vector<bool> filterResult = rbridge_applyFilter(filter, generatedFilter);

		sendFilterResult(filterResult);

		std::string RPossibleWarning = jaspRCPP_getLastFilterErrorMsg();

		if(RPossibleWarning.length() > 0)
			sendFilterError(RPossibleWarning);
	}
	catch(filterException & e)
	{
		if(std::string(e.what()).length() > 0)
			sendFilterError(e.what());
		else
			sendFilterError("Something went wrong with the filter but it is unclear what.");
	}
}


// Evaluating arbitrary R code (as string) which returns a string
void Engine::evalRCode(const string &rCode)
{
	std::string rCodeResult = rbridge_evalRCode(rCode);
	
	if (rCodeResult == "null") 
	{
		// this means an error was generated;
		std::cout << "R Code yielded error" << std::endl << std::flush;
		sendRCodeError();	
	} 
	else
	{
		std::cout << "R Code yielded result: " << rCodeResult << std::endl << std::flush;
		sendRCodeResult(rCodeResult);
	}	
}

void Engine::sendRCodeResult(std::string rCodeResult)
{
	Json::Value rCodeResponse = Json::Value(Json::objectValue);

	rCodeResponse["rCodeResult"] = rCodeResult;

	std::string msg = rCodeResponse.toStyledString();
	_channel->send(msg);
}

void Engine::sendRCodeError()
{
	Json::Value rCodeResponse = Json::Value(Json::objectValue);

	rCodeResponse["rCodeError"] = "R Code failed for unknown reason. Check that R function returns a string.";

	std::string msg = rCodeResponse.toStyledString();
	_channel->send(msg);
}
