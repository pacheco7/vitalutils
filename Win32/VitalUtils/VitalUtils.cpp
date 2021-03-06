#include "stdafx.h"
#include "VitalUtils.h"
#include "VitalUtilsDlg.h"
#include <time.h>
#include <thread>
#pragma comment(lib, "Version.lib")

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_MESSAGE_MAP(CVitalUtilsApp, CWinApp)
	ON_COMMAND(ID_HELP, &CWinApp::OnHelp)
END_MESSAGE_MAP()

CVitalUtilsApp::CVitalUtilsApp() {
	// 다시 시작 관리자 지원
	m_dwRestartManagerSupportFlags = AFX_RESTART_MANAGER_SUPPORT_RESTART;

	// TODO: 여기에 생성 코드를 추가합니다.
	// InitInstance에 모든 중요한 초기화 작업을 배치합니다.
}

CVitalUtilsApp theApp;

// create dir recursively
void CreateDir(CString path) {
	for (unsigned int pos = 0; pos != -1; pos = path.Find('\\', pos + 1)) {
		if (!pos) continue;
		auto subdir = path.Left(pos);
		HRESULT hres = CreateDirectory(subdir, NULL);
	}
}

bool FileExists(CString path) {
	CFile fil;
	if (fil.Open(path, CFile::modeRead | CFile::shareDenyNone)) { // Open succeeded, file exists 
		fil.Close();
		return true;
	} 
	if (ERROR_FILE_NOT_FOUND == ::GetLastError()) return false;
	return false;
}

CString GetModulePath() {
	TCHAR temp_path[MAX_PATH];
	GetModuleFileName(AfxGetInstanceHandle(), temp_path, sizeof(temp_path));
	return temp_path;
}

CString GetModuleDir() {
	return DirName(GetModulePath());
}

bool executeCommandLine(CString cmdLine) {
	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = NULL;
	sa.bInheritHandle = TRUE;

	STARTUPINFO si = {};
	si.cb = sizeof(si);

	PROCESS_INFORMATION pi = {};

	if (CreateProcess(NULL, (LPTSTR)(LPCTSTR)cmdLine, &sa, &sa, FALSE, 
					  CREATE_NO_WINDOW | CREATE_DEFAULT_ERROR_MODE,
					  NULL, NULL, &si, &pi) == FALSE) return false;

	// Wait until application has terminated
	WaitForSingleObject(pi.hProcess, INFINITE);

	// Close process and thread handles
	::CloseHandle(pi.hThread);
	::CloseHandle(pi.hProcess);

	return true;
}

CString GetLastErrorString() {
	//Get the error message, if any.
	DWORD errorMessageID = ::GetLastError();
	if (errorMessageID == 0)
		return CString(); //No error message has been recorded

	LPSTR messageBuffer = nullptr;
	size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
								 NULL, errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);

	CString message(messageBuffer, size);

	//Free the buffer.
	LocalFree(messageBuffer);

	return message;
}

CStringA execCmd(CString cmd) {
	CStringA strResult;
	HANDLE hPipeRead, hPipeWrite;

	SECURITY_ATTRIBUTES saAttr = { sizeof(SECURITY_ATTRIBUTES) };
	saAttr.bInheritHandle = TRUE;   //Pipe handles are inherited by child process.
	saAttr.lpSecurityDescriptor = NULL;

	// Create a pipe to get results from child's stdout.
	if (!CreatePipe(&hPipeRead, &hPipeWrite, &saAttr, 0))
		return strResult;

	STARTUPINFO si = { sizeof(STARTUPINFO) };
	si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
	si.hStdOutput = hPipeWrite;
	si.hStdError = NULL;
	si.wShowWindow = SW_HIDE;       // Prevents cmd window from flashing. Requires STARTF_USESHOWWINDOW in dwFlags.

	PROCESS_INFORMATION pi = { 0 };

	BOOL fSuccess = CreateProcess(NULL, (LPSTR)(LPCTSTR)cmd, NULL, NULL, TRUE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi);
	if (!fSuccess) {
		CloseHandle(hPipeWrite);
		CloseHandle(hPipeRead);
		return strResult;
	}

	bool bProcessEnded = false;
	for (; !bProcessEnded;) {
		// Give some timeslice (50ms), so we won't waste 100% cpu.
		bProcessEnded = WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0;

		// Even if process exited - we continue reading, if there is some data available over pipe.
		for (;;) {
			char buf[1024];
			DWORD dwRead = 0;
			DWORD dwAvail = 0;
			if (!::PeekNamedPipe(hPipeRead, NULL, 0, NULL, &dwAvail, NULL))
				break;

			if (!dwAvail) // no data available, return
				break;

			if (!::ReadFile(hPipeRead, buf, min(sizeof(buf) - 1, dwAvail), &dwRead, NULL) || !dwRead)
				// error, the child process might ended
				break;

			buf[dwRead] = 0;
			strResult += buf;
		}
	} //for

	CloseHandle(hPipeWrite);
	CloseHandle(hPipeRead);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return strResult;
}

// pipe 된 실행을 하고 출력 결과를 ofile 에 저장함
bool executeCommandLine(CString cmdLine, CString ofile) {
	auto jobid = GetThreadId(GetCurrentThread());

	// 예제 코드 https://stackoverflow.com/questions/7018228/how-do-i-redirect-output-to-a-file-with-createprocess
	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = NULL;
	sa.bInheritHandle = TRUE;

	// 파이프를 생성
	// temp file 을 연다.
	bool ret = false;
	HANDLE fo = CreateFile(ofile, GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, 
						   &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (fo == INVALID_HANDLE_VALUE) {
		CString str;
		str.Format(_T("[%d] cannot open file"), jobid);
		theApp.Log(str);
		return false;
	}

	// 에러를 출력할 파이프를 생성
	// https://msdn.microsoft.com/ko-kr/library/windows/desktop/ms682499(v=vs.85).aspx
	HANDLE stder, stdew;
	if (!CreatePipe(&stder, &stdew, &sa, 0)) {
		theApp.Log(_T("cannot create pipe"));
		return false;
	}
	if (!SetHandleInformation(stder, HANDLE_FLAG_INHERIT, 0)) {
		theApp.Log(_T("set handle information"));
		return false;
	}

	STARTUPINFO si = {};
	si.cb = sizeof(si);
	si.wShowWindow = SW_HIDE;
	si.hStdOutput = fo;
	si.hStdError = stdew;
	si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;

	PROCESS_INFORMATION pi = {};

	if (!CreateProcess(NULL, (LPTSTR)(LPCTSTR)cmdLine, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		theApp.Log(_T("createprocess error ") + GetLastErrorString());
		goto clean;
	}
	
	char buf[4096];
	while (1) {
		if (WAIT_TIMEOUT != WaitForSingleObject(pi.hProcess, 500)) break;
		for (DWORD dwAvail = 0; PeekNamedPipe(stder, 0, 0, 0, &dwAvail, 0) && dwAvail; dwAvail = 0) {
			DWORD dwRead = 0;
			ReadFile(stder, buf, min(4095, dwAvail), &dwRead, 0);
			CString str(buf, dwRead);
			theApp.Log(str);
		}
	}
	// 프로세스 종료
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);

	ret = true;

clean:
	if (fo) CloseHandle(fo);

	if (stder) CloseHandle(stder);
	if (stdew) CloseHandle(stdew); // 에러에다가 우리가 쓸 일 없음

	return ret;
}

CString ExtName(CString path) {
	int pos = path.ReverseFind('.');
	if (pos == -1) return _T("");
	return path.Mid(pos + 1).MakeLower();
}

CString DirName(CString path) {
	return path.Left(max(path.ReverseFind('/'), path.ReverseFind('\\')) + 1);
}

void ListFiles(LPCTSTR path, vector<CString>& files, CString ext) {
	int next = ext.GetLength();

	CString dirName = DirName(path);

	BOOL bSearchFinished = FALSE;
	WIN32_FIND_DATA fd = { 0, };

	HANDLE hFind = FindFirstFile(dirName + _T("*.*"), &fd);
	for (BOOL ret = (hFind != INVALID_HANDLE_VALUE); ret; ret = FindNextFile(hFind, &fd)) {
		CString filename = fd.cFileName;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { // directory
			if (filename.Left(1) == _T(".")) continue;
			ListFiles(dirName + filename, files, ext);
		} else if (filename.Right(next).MakeLower() == ext) {
			files.push_back(dirName + fd.cFileName);
		}
	}
	FindClose(hFind);
}

CString BaseName(CString path) {
	return path.Mid(max(path.ReverseFind('/'), path.ReverseFind('\\')) + 1);
}

DWORD FileTimeToUnixTime(const FILETIME &ft) {
	ULARGE_INTEGER ull;
	ull.LowPart = ft.dwLowDateTime;
	ull.HighPart = ft.dwHighDateTime;
	return (DWORD)(ull.QuadPart / 10000000ULL - 11644473600ULL);
}

UINT WINAPI WorkThread(void* p) {
	while (1) {
		DWORD_CString dstr;
		CString cmd;
		if (theApp.m_bparsing && theApp.m_parses.Pop(dstr)) { // 파징 해야할 파일이 있다면
			theApp.m_nrunning.Inc();

			auto path = dstr.second;
			auto mtime = dstr.first;
			auto cmd = GetModuleDir() + _T("utilities\\") + "vital_trks.exe -s \"" + path + "\"";
			auto res = execCmd(cmd);
			theApp.m_path_trklist[path] = make_pair(mtime, res);

			theApp.m_nrunning.Dec();
		} else if (!theApp.m_bparsing && theApp.m_jobs.Pop(cmd)) { // 추출해야할 파일이 있다면
			theApp.m_nrunning.Inc();

			auto jobid = GetThreadId(GetCurrentThread());

			CString str;
			str.Format(_T("[%d] %s"), jobid, cmd);
			theApp.Log(str);

			// 실제 프로그램을 실행
			time_t ts = time(nullptr);
			if (cmd.Left(4) == "copy") {
				auto tabs = Explode(cmd.Mid(5), '|');
				if (tabs.size() < 2) {
					theApp.Log("Copy failed! File name error");
				} else {
					auto ipath = tabs[0];
					auto opath = tabs[1];
					if (!CopyFile(ipath, opath, FALSE)) {
						theApp.Log("Copy failed! " + GetLastErrorString());
					}
				}
			} else {
				cmd = GetModuleDir() + _T("utilities\\") + cmd;
				int dred = cmd.Find(">>");
				int red = cmd.Find('>');
				if (dred >= 1) { // Longitudinal file
					CString ofile = cmd.Mid(dred + 2);
					ofile = ofile.Trim(_T("\t \""));
					cmd = cmd.Left(dred);
					auto s = execCmd(cmd);
					
					// lock and save the result
					EnterCriticalSection(&theApp.m_csLong);
					FILE* fa = fopen(ofile, "ab"); // append file
					s.TrimRight("\r\n");
					fwrite(s, 1, s.GetLength(), fa);
					fprintf(fa, "\r\n");
					fclose(fa);
					LeaveCriticalSection(&theApp.m_csLong);
				} else if (red >= 1) { // 파이프 실행
					CString ofile = cmd.Mid(red + 1);
					ofile = ofile.Trim(_T("\t \""));
					cmd = cmd.Left(red);
					executeCommandLine(cmd, ofile);
				} else { // 단순 실행
					executeCommandLine(cmd);
				}
			}

			time_t te = time(nullptr);
			str.Format(_T("[%d] finished in %d sec"), jobid, (int)difftime(te, ts));
			theApp.Log(str);

			theApp.m_nrunning.Dec();
		} else {
			Sleep(100);
		}
	}
	return 0;
}

CString GetProductVer() {
	auto temp_path = GetModulePath();

	// 실행파일의 버전정보는 세부항목이 추가나 삭제될수 있기 때문에 크기가 일정하지 않습니다.따라서 먼저
	// 버전정보의 크기를 얻은후에 그 크기만큼 메모리를 할당하고 버전정보를 해당 메모리에 복사해야 합니다.
	// 버전 정보를 얻기 위해 사용할 핸들값을 저장하는 변수이다.
	DWORD h_version_handle;
	// 버전정보는 항목을 사용자가 추가/삭제 할수 있기 때문에 고정된 크기가 아니다.
	// 따라서 현재 프로그램의 버전정보에 대한 크기를 얻어서 그 크기에 맞는 메모리를 할당하고 작업해야한다.
	DWORD version_info_size = GetFileVersionInfoSize(temp_path, &h_version_handle);

	// 버전정보를 저장하기 위한 시스템 메모리를 생성한다. ( 핸들 형식으로 생성 )
	HANDLE h_memory = GlobalAlloc(GMEM_MOVEABLE, version_info_size);
	// 핸들 형식의 메모리를 사용하기 위해서 해당 핸들에 접근할수 있는 주소를 얻는다.
	LPVOID p_info_memory = GlobalLock(h_memory);

	// 현재 프로그램의 버전 정보를 가져온다.
	GetFileVersionInfo(temp_path, h_version_handle, version_info_size, p_info_memory);

	// 버전 정보에 포함된 각 항목별 정보 위치를 저장할 변수이다. 이 포인터에 전달된 주소는
	// p_info_memory 의 내부 위치이기 때문에 해제하면 안됩니다.
	// ( p_info_memory 를 참조하는 형식의 포인터 입니다. )
	char *p_data = NULL;

	// 실제로 읽은 정보의 크기를 저장할 변수이다.
	UINT data_size = 0;

	// 세부항목 명시에 사용된 041204b0 는 언어코드이고 "Korean"를 의미합니다.
	// 버전정보에 포함된 Comments 정보를 얻어서 출력합니다.
	//VerQueryValue(p_info_memory, "\\StringFileInfo\\041204b0\\Comments", (void **)&p_data, &data_size);

	// 버전정보에 포함된 CompanyName 정보를 얻어서 출력한다.
	//VerQueryValue(p_info_memory, "\\StringFileInfo\\041204b0\\CompanyName", (void **)&p_data, &data_size);

	// 버전정보에 포함된 FileDescription 정보를 얻어서 출력한다.
	//VerQueryValue(p_info_memory, "\\StringFileInfo\\041204b0\\FileDescription", (void **)&p_data, &data_size);

	// 버전정보에 포함된 FileVersion 정보를 얻어서 출력한다.
	VerQueryValue(p_info_memory, "\\StringFileInfo\\000904b0\\ProductVersion", (void **)&p_data, &data_size);

	// 버전 정보를 저장하기 위해 사용했던 메모리를 해제한다.
	GlobalUnlock(h_memory);
	GlobalFree(h_memory);

	return p_data;
}

BOOL CVitalUtilsApp::InitInstance() {
	// 응용 프로그램 매니페스트가 ComCtl32.dll 버전 6 이상을 사용하여 비주얼 스타일을
	// 사용하도록 지정하는 경우, Windows XP 상에서 반드시 InitCommonControlsEx()가 필요합니다.
	// InitCommonControlsEx()를 사용하지 않으면 창을 만들 수 없습니다.
	INITCOMMONCONTROLSEX InitCtrls;
	InitCtrls.dwSize = sizeof(InitCtrls);
	// 응용 프로그램에서 사용할 모든 공용 컨트롤 클래스를 포함하도록
	// 이 항목을 설정하십시오.
	InitCtrls.dwICC = ICC_WIN95_CLASSES;
	InitCommonControlsEx(&InitCtrls);

	CWinApp::InitInstance();

	if (!FileExists(GetModuleDir() + _T("utilities\\vital_trks.exe")) || (!FileExists(GetModuleDir() + _T("utilities\\vital_recs.exe")) && !FileExists(GetModuleDir() + _T("utilities\\vital_recs_x64.exe")))) {
		AfxMessageBox(_T("vital_recs.exe and vital_trks.exe should be exist in utilities folder"));
		return FALSE;
	}

	AfxEnableControlContainer();
	AfxInitRichEdit();

	// 대화 상자에 셸 트리 뷰 또는
	// 셸 목록 뷰 컨트롤이 포함되어 있는 경우 셸 관리자를 만듭니다.
	CShellManager *pShellManager = new CShellManager;

	// MFC 컨트롤의 테마를 사용하기 위해 "Windows 원형" 비주얼 관리자 활성화
	CMFCVisualManager::SetDefaultManager(RUNTIME_CLASS(CMFCVisualManagerWindows));

	SetRegistryKey(_T("VitalUtils"));

	InitializeCriticalSection(&m_csFile);
	InitializeCriticalSection(&m_csTrk);
	InitializeCriticalSection(&m_csLong);

	// begin worker thread pool
	auto ncores = (int)thread::hardware_concurrency();
	for (int i = 0; i < ncores; ++i)
		_beginthreadex(0, 0, WorkThread, 0, 0, 0);

	CVitalUtilsDlg dlg;
	m_pMainWnd = &dlg;
	INT_PTR nResponse = dlg.DoModal();
	if (nResponse == IDOK) {
		// TODO: 여기에 [확인]을 클릭하여 대화 상자가 없음질 때 처리할
		//  코드를 배치합니다.
	} else if (nResponse == IDCANCEL) {
		// TODO: 여기에 [취소]를 클릭하여 대화 상자가 없음질 때 처리할
		//  코드를 배치합니다.
	} else if (nResponse == -1) {
	}

	// 위에서 만든 셸 관리자를 삭제합니다.
	if (pShellManager != NULL) {
		delete pShellManager;
	}

#if !defined(_AFXDLL) && !defined(_AFX_NO_MFC_CONTROLS_IN_DIALOGS)
	ControlBarCleanUp();
#endif

	// 대화 상자가 닫혔으므로 응용 프로그램의 메시지 펌프를 시작하지 않고  응용 프로그램을 끝낼 수 있도록 FALSE를
	// 반환합니다.
	return FALSE;
}

void CVitalUtilsApp::Log(CString msg) {
	m_msgs.Push(make_pair((DWORD)time(nullptr), msg));
}

void CVitalUtilsApp::AddJob(CString cmd) {
	m_jobs.Push(cmd);
	m_ntotal++;
}

int CVitalUtilsApp::ExitInstance() {
	EnterCriticalSection(&m_csFile);
	
	auto copy = m_files;
	m_files.clear();
	LeaveCriticalSection(&m_csFile);
	for (auto& p : copy)
		delete p;

	DeleteCriticalSection(&m_csTrk);
	DeleteCriticalSection(&m_csFile);
	DeleteCriticalSection(&m_csLong);

	return CWinApp::ExitInstance();
}

vector<CString> Explode(CString str, TCHAR sep) {
	vector<CString> ret;
	if (str.IsEmpty()) return ret;
	if (str[str.GetLength() - 1] != sep) str += sep;
	for (int i = 0, j = 0; (j = str.Find(sep, i)) >= 0; i = j + 1) {
		CString tmp = str.Mid(i, j - i);
		tmp.TrimLeft(sep); tmp.TrimRight(sep);
		ret.push_back(tmp);
	}
	if (ret.empty()) {
		str.TrimLeft(sep); str.TrimRight(sep);
		ret.push_back(str);
	}
	return ret;
}

void CVitalUtilsApp::SaveCache(CString cachepath) { // 새 캐쉬를 저장
	Log("Saving cache");

	char temppath[MAX_PATH] = { 0 };
	char tempdir[MAX_PATH] = { 0 };
	if (!GetTempPath(MAX_PATH, tempdir))
		return;

	GetTempFileName(tempdir, "trks", (DWORD)time(nullptr), temppath);
	
	CString idir = DirName(cachepath); // 마지막 \ 포함
	auto idir_len = idir.GetLength();

	// 다른 스래드에서 사용중이므로 일단 복사
	auto copyed = m_path_trklist;
	FILE* fo = fopen(temppath, "wt");
	if (!fo) {
		Log("Saving cache failed! " + GetLastErrorString());
		return;
	}
	
	// 상대 경로, 시간, 트랙 저장
	for (const auto& it : copyed) {
		fprintf(fo, "%s\t%u\t%s\n", it.first.Mid(idir_len), it.second.first, it.second.second);
	}
	fclose(fo);

	if (!MoveFileEx(temppath, cachepath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
		Log("Saving cache failed! " + GetLastErrorString());
		return;
	}
	DeleteFile(temppath);

	Log("Saving done");
}
