
#include "odbcimpl.h"
#include <algorithm>
#include <sstream>
#ifndef NDEBUG
#include <iostream>
#endif

namespace SPA
{
    namespace ServerSide{

        const wchar_t * COdbcImpl::NO_DB_OPENED_YET = L"No ODBC database opened yet";
        const wchar_t * COdbcImpl::BAD_END_TRANSTACTION_PLAN = L"Bad end transaction plan";
        const wchar_t * COdbcImpl::NO_PARAMETER_SPECIFIED = L"No parameter specified";
        const wchar_t * COdbcImpl::BAD_PARAMETER_DATA_ARRAY_SIZE = L"Bad parameter data array length";
        const wchar_t * COdbcImpl::BAD_PARAMETER_COLUMN_SIZE = L"Bad parameter column size";
        const wchar_t * COdbcImpl::DATA_TYPE_NOT_SUPPORTED = L"Data type not supported";
        const wchar_t * COdbcImpl::NO_DB_NAME_SPECIFIED = L"No database name specified";
        const wchar_t * COdbcImpl::ODBC_ENVIRONMENT_NOT_INITIALIZED = L"ODBC system library not initialized";
        const wchar_t * COdbcImpl::BAD_MANUAL_TRANSACTION_STATE = L"Bad manual transaction state";
        const wchar_t * COdbcImpl::BAD_INPUT_PARAMETER_DATA_TYPE = L"Bad input parameter data type";
        const wchar_t * COdbcImpl::BAD_PARAMETER_DIRECTION_TYPE = L"Bad parameter direction type";

        SQLHENV COdbcImpl::g_hEnv = nullptr;

        const wchar_t * COdbcImpl::ODBC_GLOBAL_CONNECTION_STRING = L"ODBC_GLOBAL_CONNECTION_STRING";

        CUCriticalSection COdbcImpl::m_csPeer;
        std::wstring COdbcImpl::m_strGlobalConnection;

        bool COdbcImpl::SetODBCEnv(int param) {
            SQLRETURN retcode = SQLSetEnvAttr(nullptr, SQL_ATTR_CONNECTION_POOLING, (SQLPOINTER) SQL_CP_ONE_PER_DRIVER, SQL_IS_INTEGER);
            retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &g_hEnv);
            if (!SQL_SUCCEEDED(retcode))
                return false;
            retcode = SQLSetEnvAttr(g_hEnv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER*) SQL_OV_ODBC3, 0);
            if (!SQL_SUCCEEDED(retcode))
                return false;
            return true;
        }

        void COdbcImpl::FreeODBCEnv() {
            if (g_hEnv) {
                SQLFreeHandle(SQL_HANDLE_ENV, g_hEnv);
                g_hEnv = nullptr;
            }
        }

        void COdbcImpl::SetGlobalConnectionString(const wchar_t * str) {
            m_csPeer.lock();
            if (str)
                m_strGlobalConnection = str;
            else
                m_strGlobalConnection.clear();
            m_csPeer.unlock();
        }

        void COdbcImpl::ODBC_CONNECTION_STRING::Trim(std::wstring & s) {
            static const wchar_t *WHITESPACE = L" \r\n\t\v\f\v";
            auto pos = s.find_first_of(WHITESPACE);
            while (pos == 0) {
                s.erase(s.begin());
                pos = s.find_first_of(WHITESPACE);
            }
            pos = s.find_last_of(WHITESPACE);
            while (s.size() && pos == s.size() - 1) {
                s.pop_back();
                pos = s.find_last_of(WHITESPACE);
            }
        }

        void COdbcImpl::ODBC_CONNECTION_STRING::Parse(const wchar_t * s) {
            using namespace std;
            if (!wcsstr(s, L"="))
                return;
            wstringstream ss(s ? s : L"");
            wstring item;
            vector<wstring> tokens;
            while (getline(ss, item, L';')) {
                tokens.push_back(item);
            }
            for (auto it = tokens.begin(), end = tokens.end(); it != end; ++it) {
                auto pos = it->find(L'=');
                if (pos == string::npos)
                    continue;
                wstring left = it->substr(0, pos);
                wstring right = it->substr(pos + 1);
                Trim(left);
                Trim(right);
                transform(left.begin(), left.end(), left.begin(), ::tolower);
                if (left == L"connect-timeout" || left == L"timeout" || left == L"connection-timeout") {
#ifdef WIN32_64
                    timeout = (unsigned int) _wtoi(right.c_str());
#else
                    wchar_t *tail = nullptr;
                    timeout = (unsigned int) wcstol(right.c_str(), &tail, 0);
#endif
                } else if (left == L"database" || left == L"db")
                    database = right;
                else if (left == L"port") {
#ifdef WIN32_64
                    port = (unsigned int) _wtoi(right.c_str());
#else
                    wchar_t *tail = nullptr;
                    port = (unsigned int) wcstol(right.c_str(), &tail, 0);
#endif
                } else if (left == L"pwd" || left == L"password") {
                    password = right;
                } else if (left == L"host" || left == L"server" || left == L"dsn") {
                    host = right;
                } else if (left == L"user" || left == L"uid") {
                    user = right;
                } else if (left == L"async" || left == L"asynchronous") {
#ifdef WIN32_64
                    async = (_wtoi(right.c_str()) ? true : false);
#else
                    wchar_t *tail = nullptr;
                    async = (wcstol(right.c_str(), &tail, 0) ? true : false);
#endif
                } else {
                    //!!! not implemented
                    assert(false);
                }
            }
        }

        COdbcImpl::COdbcImpl()
        : m_oks(0), m_fails(0), m_ti(tiUnspecified), m_global(true),
        m_Blob(*m_sb), m_parameters(0), m_bCall(false), m_bReturn(false),
        m_outputs(0), m_nParamDataSize(0) {

        }

        void COdbcImpl::OnReleaseSource(bool bClosing, unsigned int info) {
            CleanDBObjects();
            m_Blob.SetSize(0);
            if (m_Blob.GetMaxSize() > 2 * DEFAULT_BIG_FIELD_CHUNK_SIZE) {
                m_Blob.ReallocBuffer(2 * DEFAULT_BIG_FIELD_CHUNK_SIZE);
            }
            m_global = true;
            m_pExcuting.reset();
        }

        void COdbcImpl::OnSwitchFrom(unsigned int nOldServiceId) {
            m_oks = 0;
            m_fails = 0;
            m_ti = tiUnspecified;
        }

        void COdbcImpl::OnFastRequestArrive(unsigned short reqId, unsigned int len) {
            BEGIN_SWITCH(reqId)
            M_I1_R0(idStartBLOB, StartBLOB, unsigned int)
            M_I0_R0(idChunk, Chunk)
            M_I0_R0(idEndBLOB, EndBLOB)
            M_I0_R0(idBeginRows, BeginRows)
            M_I0_R0(idTransferring, Transferring)
            M_I0_R0(idEndRows, EndRows)
            M_I0_R2(idClose, CloseDb, int, std::wstring)
            M_I2_R3(idOpen, Open, std::wstring, unsigned int, int, std::wstring, int)
            M_I3_R3(idBeginTrans, BeginTrans, int, std::wstring, unsigned int, int, std::wstring, int)
            M_I1_R2(idEndTrans, EndTrans, int, int, std::wstring)
            M_I5_R5(idExecute, Execute, std::wstring, bool, bool, bool, UINT64, INT64, int, std::wstring, CDBVariant, UINT64)
            M_I2_R3(idPrepare, Prepare, std::wstring, CParameterInfoArray, int, std::wstring, unsigned int)
            M_I4_R5(idExecuteParameters, ExecuteParameters, bool, bool, bool, UINT64, INT64, int, std::wstring, CDBVariant, UINT64)
#if 0
            M_I5_R3(SPA::Odbc::idSQLColumnPrivileges, DoSQLColumnPrivileges, std::wstring, std::wstring, std::wstring, std::wstring, UINT64, int, std::wstring, UINT64)
            M_I5_R3(SPA::Odbc::idSQLColumns, DoSQLColumns, std::wstring, std::wstring, std::wstring, std::wstring, UINT64, int, std::wstring, UINT64)
            M_I7_R3(SPA::Odbc::idSQLForeignKeys, DoSQLForeignKeys, std::wstring, std::wstring, std::wstring, std::wstring, std::wstring, std::wstring, UINT64, int, std::wstring, UINT64)
            M_I4_R3(SPA::Odbc::idSQLPrimaryKeys, DoSQLPrimaryKeys, std::wstring, std::wstring, std::wstring, UINT64, int, std::wstring, UINT64)
            M_I5_R3(SPA::Odbc::idSQLProcedureColumns, DoSQLProcedureColumns, std::wstring, std::wstring, std::wstring, std::wstring, UINT64, int, std::wstring, UINT64)
            M_I4_R3(SPA::Odbc::idSQLProcedures, DoSQLProcedures, std::wstring, std::wstring, std::wstring, UINT64, int, std::wstring, UINT64)
            M_I7_R3(SPA::Odbc::idSQLSpecialColumns, DoSQLSpecialColumns, SQLSMALLINT, std::wstring, std::wstring, std::wstring, SQLSMALLINT, SQLSMALLINT, UINT64, int, std::wstring, UINT64)
            M_I6_R3(SPA::Odbc::idSQLStatistics, DoSQLStatistics, std::wstring, std::wstring, std::wstring, SQLUSMALLINT, SQLUSMALLINT, UINT64, int, std::wstring, UINT64)
            M_I4_R3(SPA::Odbc::idSQLTablePrivileges, DoSQLTablePrivileges, std::wstring, std::wstring, std::wstring, UINT64, int, std::wstring, UINT64)
            M_I5_R3(SPA::Odbc::idSQLTables, DoSQLTables, std::wstring, std::wstring, std::wstring, std::wstring, UINT64, int, std::wstring, UINT64)
#endif
            END_SWITCH
            m_pExcuting.reset();
            if (reqId == idExecuteParameters) {
                ReleaseArray();
                m_vParam.clear();
            }
        }

        int COdbcImpl::OnSlowRequestArrive(unsigned short reqId, unsigned int len) {
            BEGIN_SWITCH(reqId)
            M_I0_R2(idClose, CloseDb, int, std::wstring)
            M_I2_R3(idOpen, Open, std::wstring, unsigned int, int, std::wstring, int)
            M_I3_R3(idBeginTrans, BeginTrans, int, std::wstring, unsigned int, int, std::wstring, int)
            M_I1_R2(idEndTrans, EndTrans, int, int, std::wstring)
            M_I5_R5(idExecute, Execute, std::wstring, bool, bool, bool, UINT64, INT64, int, std::wstring, CDBVariant, UINT64)
            M_I2_R3(idPrepare, Prepare, std::wstring, CParameterInfoArray, int, std::wstring, unsigned int)
            M_I4_R5(idExecuteParameters, ExecuteParameters, bool, bool, bool, UINT64, INT64, int, std::wstring, CDBVariant, UINT64)
            M_I5_R3(SPA::Odbc::idSQLColumnPrivileges, DoSQLColumnPrivileges, std::wstring, std::wstring, std::wstring, std::wstring, UINT64, int, std::wstring, UINT64)
            M_I5_R3(SPA::Odbc::idSQLColumns, DoSQLColumns, std::wstring, std::wstring, std::wstring, std::wstring, UINT64, int, std::wstring, UINT64)
            M_I7_R3(SPA::Odbc::idSQLForeignKeys, DoSQLForeignKeys, std::wstring, std::wstring, std::wstring, std::wstring, std::wstring, std::wstring, UINT64, int, std::wstring, UINT64)
            M_I4_R3(SPA::Odbc::idSQLPrimaryKeys, DoSQLPrimaryKeys, std::wstring, std::wstring, std::wstring, UINT64, int, std::wstring, UINT64)
            M_I5_R3(SPA::Odbc::idSQLProcedureColumns, DoSQLProcedureColumns, std::wstring, std::wstring, std::wstring, std::wstring, UINT64, int, std::wstring, UINT64)
            M_I4_R3(SPA::Odbc::idSQLProcedures, DoSQLProcedures, std::wstring, std::wstring, std::wstring, UINT64, int, std::wstring, UINT64)
            M_I7_R3(SPA::Odbc::idSQLSpecialColumns, DoSQLSpecialColumns, SQLSMALLINT, std::wstring, std::wstring, std::wstring, SQLSMALLINT, SQLSMALLINT, UINT64, int, std::wstring, UINT64)
            M_I6_R3(SPA::Odbc::idSQLStatistics, DoSQLStatistics, std::wstring, std::wstring, std::wstring, SQLUSMALLINT, SQLUSMALLINT, UINT64, int, std::wstring, UINT64)
            M_I4_R3(SPA::Odbc::idSQLTablePrivileges, DoSQLTablePrivileges, std::wstring, std::wstring, std::wstring, UINT64, int, std::wstring, UINT64)
            M_I5_R3(SPA::Odbc::idSQLTables, DoSQLTables, std::wstring, std::wstring, std::wstring, std::wstring, UINT64, int, std::wstring, UINT64)
            END_SWITCH
            m_pExcuting.reset();
            if (reqId == idExecuteParameters) {
                ReleaseArray();
                m_vParam.clear();
            }
            return 0;
        }

        void COdbcImpl::OnBaseRequestArrive(unsigned short requestId) {
            switch (requestId) {
                case idCancel:
#ifndef NDEBUG
                    std::cout << "Cancel called" << std::endl;
#endif
                    do {
                        std::shared_ptr<void> pExcuting = m_pExcuting;
                        if (!pExcuting)
                            break;
                        SQLRETURN retcode = SQLCancel(pExcuting.get());
                        if (m_ti == tiUnspecified)
                            break;
                        retcode = SQLEndTran(SQL_HANDLE_DBC, m_pOdbc.get(), SQL_ROLLBACK);
                        m_ti = tiUnspecified;
                    } while (false);
                    break;
                default:
                    break;
            }
        }

        void COdbcImpl::ReleaseArray() {
            for (auto it = m_vArray.begin(), end = m_vArray.end(); it != end; ++it) {
                SAFEARRAY *arr = *it;
                ::SafeArrayUnaccessData(arr);
            }
            m_vArray.clear();
        }

        void COdbcImpl::CleanDBObjects() {
            m_pPrepare.reset();
            m_pOdbc.reset();
            m_vParam.clear();
        }

        void COdbcImpl::Open(const std::wstring &strConnection, unsigned int flags, int &res, std::wstring &errMsg, int &ms) {
            ms = msODBC;
            CleanDBObjects();
            if (!g_hEnv) {
                res = SPA::Odbc::ER_ODBC_ENVIRONMENT_NOT_INITIALIZED;
                errMsg = ODBC_ENVIRONMENT_NOT_INITIALIZED;
                return;
            } else {
                res = 0;
            }
            do {
                std::wstring db(strConnection);
                if (!db.size() || db == ODBC_GLOBAL_CONNECTION_STRING) {
                    m_csPeer.lock();
                    db = m_strGlobalConnection;
                    m_csPeer.unlock();
                    m_global = true;
                } else {
                    m_global = false;
                }
                ODBC_CONNECTION_STRING ocs;
                ocs.Parse(db.c_str());
                SQLHDBC hdbc = nullptr;
                SQLRETURN retcode = SQLAllocHandle(SQL_HANDLE_DBC, g_hEnv, &hdbc);
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_ENV, g_hEnv, errMsg);
                    break;
                }

                if (ocs.timeout) {
                    SQLPOINTER rgbValue = &ocs.timeout;
                    retcode = SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, rgbValue, 0);
                }

                if (ocs.database.size()) {
					std::string db = SPA::Utilities::ToUTF8(ocs.database.c_str(), ocs.database.size());
                    retcode = SQLSetConnectAttr(hdbc, SQL_ATTR_CURRENT_CATALOG, (SQLPOINTER) db.c_str(), (SQLINTEGER) (db.size() * sizeof (SQLCHAR)));
                }

                retcode = SQLSetConnectAttr(hdbc, SQL_ATTR_ASYNC_ENABLE, (SQLPOINTER) (ocs.async ? SQL_ASYNC_ENABLE_ON : SQL_ASYNC_ENABLE_OFF), 0);

				std::string host = SPA::Utilities::ToUTF8(ocs.host.c_str(), ocs.host.size());
				std::string user = SPA::Utilities::ToUTF8(ocs.user.c_str(), ocs.user.size());
				std::string pwd = SPA::Utilities::ToUTF8(ocs.password.c_str(), ocs.password.size());

                retcode = SQLConnect(hdbc, (SQLCHAR*) host.c_str(), (SQLSMALLINT) host.size(), (SQLCHAR *) user.c_str(), (SQLSMALLINT) user.size(), (SQLCHAR *) pwd.c_str(), (SQLSMALLINT) pwd.size());
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_DBC, hdbc, errMsg);
                    SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
                    break;
                }
                m_pOdbc.reset(hdbc, [](SQLHDBC h) {
                    if (h) {
                        SQLRETURN ret = SQLDisconnect(h);
                        assert(ret == SQL_SUCCESS);
                        ret = SQLFreeHandle(SQL_HANDLE_DBC, h);
                        assert(ret == SQL_SUCCESS);
                    }
                });
                PushInfo(hdbc);
            } while (false);
        }

        void COdbcImpl::CloseDb(int &res, std::wstring & errMsg) {
            CleanDBObjects();
            res = 0;
        }

        void COdbcImpl::BeginTrans(int isolation, const std::wstring &dbConn, unsigned int flags, int &res, std::wstring &errMsg, int &ms) {
            ms = msODBC;
            if (m_ti != tiUnspecified || isolation == (int) tiUnspecified) {
                errMsg = BAD_MANUAL_TRANSACTION_STATE;
                res = SPA::Odbc::ER_BAD_MANUAL_TRANSACTION_STATE;
                return;
            }
            if (!m_pOdbc) {
                Open(dbConn, flags, res, errMsg, ms);
                if (!m_pOdbc) {
                    return;
                }
            }
            SQLINTEGER attr;
            switch ((tagTransactionIsolation) isolation) {
                case tiReadUncommited:
                    attr = SQL_TXN_READ_UNCOMMITTED;
                    break;
                case tiRepeatableRead:
                    attr = SQL_TXN_REPEATABLE_READ;
                    break;
                case tiReadCommited:
                    attr = SQL_TXN_READ_COMMITTED;
                    break;
                case tiSerializable:
                    attr = SQL_TXN_SERIALIZABLE;
                    break;
                default:
                    attr = 0;
                    break;
            }
            SQLRETURN retcode;
            if (attr) {
                retcode = SQLSetConnectAttr(m_pOdbc.get(), SQL_ATTR_TXN_ISOLATION, (SQLPOINTER) attr, 0);
                //ignore errors
            }
            retcode = SQLSetConnectAttr(m_pOdbc.get(), SQL_ATTR_AUTOCOMMIT, (SQLPOINTER) SQL_FALSE, 0);
            if (!SQL_SUCCEEDED(retcode)) {
                res = SPA::Odbc::ER_ERROR;
                GetErrMsg(SQL_HANDLE_DBC, m_pOdbc.get(), errMsg);
            } else {
                res = 0;
                m_fails = 0;
                m_oks = 0;
                m_ti = (tagTransactionIsolation) isolation;
                if (!m_global) {
                    errMsg = dbConn;
                } else {
                    errMsg = ODBC_GLOBAL_CONNECTION_STRING;
                }
            }
        }

        void COdbcImpl::EndTrans(int plan, int &res, std::wstring & errMsg) {
            if (m_ti == tiUnspecified) {
                errMsg = BAD_MANUAL_TRANSACTION_STATE;
                res = SPA::Odbc::ER_BAD_MANUAL_TRANSACTION_STATE;
                return;
            }
            if (plan < 0 || plan > rpRollbackAlways) {
                res = SPA::Odbc::ER_BAD_END_TRANSTACTION_PLAN;
                errMsg = BAD_END_TRANSTACTION_PLAN;
                return;
            }
            if (!m_pOdbc) {
                res = SPA::Odbc::ER_NO_DB_OPENED_YET;
                errMsg = NO_DB_OPENED_YET;
                return;
            }
            bool rollback = false;
            tagRollbackPlan rp = (tagRollbackPlan) plan;
            switch (rp) {
                case rpRollbackErrorAny:
                    rollback = m_fails ? true : false;
                    break;
                case rpRollbackErrorLess:
                    rollback = (m_fails < m_oks && m_fails) ? true : false;
                    break;
                case rpRollbackErrorEqual:
                    rollback = (m_fails >= m_oks) ? true : false;
                    break;
                case rpRollbackErrorMore:
                    rollback = (m_fails > m_oks) ? true : false;
                    break;
                case rpRollbackErrorAll:
                    rollback = (m_oks) ? false : true;
                    break;
                case rpRollbackAlways:
                    rollback = true;
                    break;
                default:
                    assert(false); //shouldn't come here
                    break;
            }
            SQLRETURN retcode = SQLEndTran(SQL_HANDLE_DBC, m_pOdbc.get(), rollback ? SQL_ROLLBACK : SQL_COMMIT);
            if (!SQL_SUCCEEDED(retcode)) {
                res = SPA::Odbc::ER_ERROR;
                GetErrMsg(SQL_HANDLE_DBC, m_pOdbc.get(), errMsg);
            } else {
                res = SQL_SUCCESS;
                m_ti = tiUnspecified;
                m_fails = 0;
                m_oks = 0;
            }
        }

        bool COdbcImpl::SendRows(CUQueue& sb, bool transferring) {
            bool batching = (GetBytesBatched() >= DEFAULT_RECORD_BATCH_SIZE);
            if (batching) {
                CommitBatching();
            }
            unsigned int ret = SendResult(transferring ? idTransferring : idEndRows, sb.GetBuffer(), sb.GetSize());
            sb.SetSize(0);
            if (batching) {
                StartBatching();
            }
            if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                return false;
            }
            return true;
        }

        bool COdbcImpl::SendBlob(unsigned short data_type, const unsigned char *buffer, unsigned int bytes) {
            unsigned int ret = SendResult(idStartBLOB,
            (unsigned int) (bytes + sizeof (unsigned short) + sizeof (unsigned int) + sizeof (unsigned int))/* extra 4 bytes for string null termination*/,
            data_type, bytes);
            if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                return false;
            }
            while (bytes > DEFAULT_BIG_FIELD_CHUNK_SIZE) {
                ret = SendResult(idChunk, buffer, DEFAULT_BIG_FIELD_CHUNK_SIZE);
                if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                    return false;
                }
                assert(ret == DEFAULT_BIG_FIELD_CHUNK_SIZE);
                buffer += DEFAULT_BIG_FIELD_CHUNK_SIZE;
                bytes -= DEFAULT_BIG_FIELD_CHUNK_SIZE;
            }
            ret = SendResult(idEndBLOB, buffer, bytes);
            if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                return false;
            }
            return true;
        }

        bool COdbcImpl::SendUText(SQLHSTMT hstmt, SQLUSMALLINT index, CUQueue &qTemp, CUQueue &q, bool & blob) {
            qTemp.SetSize(0);
            SQLLEN len_or_null = 0;
            SQLRETURN retcode = SQLGetData(hstmt, index, SQL_C_WCHAR, (SQLPOINTER) qTemp.GetBuffer(), qTemp.GetMaxSize(), &len_or_null);
            assert(SQL_SUCCEEDED(retcode));
            if (len_or_null == SQL_NULL_DATA) {
                q << (VARTYPE) VT_NULL;
                blob = false;
                return true;
            } else if ((unsigned int) len_or_null < qTemp.GetMaxSize()) {
#ifdef WIN32_64
                q << (VARTYPE) VT_BSTR << (unsigned int) len_or_null;
                q.Push(qTemp.GetBuffer(), (unsigned int) len_or_null);
#else

#endif
                blob = false;
                return true;
            }
            if (q.GetSize() && !SendRows(q, true)) {
                return false;
            }
#ifdef WIN32_64
            unsigned int bytes = (unsigned int) len_or_null;
            unsigned int ret = SendResult(idStartBLOB, (unsigned int) (bytes + sizeof (VARTYPE) + sizeof (unsigned int) + sizeof (unsigned int)), (VARTYPE) VT_BSTR, bytes);
            if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                return false;
            }
            blob = true;
            while (retcode == SQL_SUCCESS_WITH_INFO) {
                if (bytes > qTemp.GetMaxSize()) {
                    bytes = qTemp.GetMaxSize();
                }
                bytes -= sizeof (wchar_t);
                ret = SendResult(idChunk, qTemp.GetBuffer(), bytes);
                if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                    return false;
                }
                retcode = SQLGetData(hstmt, index, SQL_C_WCHAR, (SQLPOINTER) qTemp.GetBuffer(), qTemp.GetMaxSize(), &len_or_null);
                bytes = (unsigned int) len_or_null;
            }
            ret = SendResult(idEndBLOB, qTemp.GetBuffer(), bytes);
            if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                return false;
            }
#else

#endif
            return true;
        }

        bool COdbcImpl::SendBlob(SQLHSTMT hstmt, SQLUSMALLINT index, VARTYPE vt, CUQueue &qTemp, CUQueue &q, bool & blob) {
            qTemp.SetSize(0);
            SQLLEN len_or_null = 0;
            SQLRETURN retcode = SQLGetData(hstmt, index, SQL_C_BINARY, (SQLPOINTER) qTemp.GetBuffer(), qTemp.GetMaxSize(), &len_or_null);
            assert(SQL_SUCCEEDED(retcode));
            if (len_or_null == SQL_NULL_DATA) {
                q << (VARTYPE) VT_NULL;
                blob = false;
                return true;
            } else if ((unsigned int) len_or_null <= qTemp.GetMaxSize()) {
                q << vt << (unsigned int) len_or_null;
                q.Push(qTemp.GetBuffer(), (unsigned int) len_or_null);
                blob = false;
                return true;
            }
            if (q.GetSize() && !SendRows(q, true)) {
                return false;
            }
            unsigned int bytes = (unsigned int) len_or_null;
            unsigned int ret = SendResult(idStartBLOB, (unsigned int) (bytes + sizeof (VARTYPE) + sizeof (unsigned int) + sizeof (unsigned int)), vt, bytes);
            if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                return false;
            }
            blob = true;
            while (retcode == SQL_SUCCESS_WITH_INFO) {
                if (bytes > qTemp.GetMaxSize()) {
                    bytes = qTemp.GetMaxSize();
                }
                ret = SendResult(idChunk, qTemp.GetBuffer(), bytes);
                if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                    return false;
                }
                retcode = SQLGetData(hstmt, index, SQL_C_BINARY, (SQLPOINTER) qTemp.GetBuffer(), qTemp.GetMaxSize(), &len_or_null);
                bytes = (unsigned int) len_or_null;
            }
            ret = SendResult(idEndBLOB, qTemp.GetBuffer(), bytes);
            if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                return false;
            }
            return true;
        }

        CDBColumnInfoArray COdbcImpl::GetColInfo(SQLHSTMT hstmt, SQLSMALLINT columns, bool meta) {
            bool primary_key_set = false;
            SQLCHAR colname[128 + 1] = {0}; // column name
            SQLSMALLINT colnamelen = 0; // length of column name
            SQLSMALLINT nullable = 0; // whether column can have NULL value
            SQLULEN collen = 0; // column lengths
            SQLSMALLINT coltype = 0; // column type
            SQLSMALLINT decimaldigits = 0; // no of digits if column is numeric
            SQLLEN displaysize = 0; // drivers column display size
            CDBColumnInfoArray vCols;
            for (SQLSMALLINT n = 0; n < columns; ++n) {
                vCols.push_back(CDBColumnInfo());
                CDBColumnInfo &info = vCols.back();
                SQLRETURN retcode = SQLDescribeCol(hstmt, (SQLUSMALLINT) (n + 1), colname, sizeof (colname) / sizeof (SQLCHAR), &colnamelen, &coltype, &collen, &decimaldigits, &nullable);
                assert(SQL_SUCCEEDED(retcode));
                info.DisplayName = SPA::Utilities::ToWide((const char*)colname, (size_t)colnamelen); //display column name
                if (nullable == SQL_NO_NULLS) {
                    info.Flags |= CDBColumnInfo::FLAG_NOT_NULL;
                }

                retcode = SQLColAttribute(hstmt, (SQLUSMALLINT) (n + 1), SQL_DESC_BASE_COLUMN_NAME, colname, sizeof (colname), &colnamelen, &displaysize);
                assert(SQL_SUCCEEDED(retcode));
                info.OriginalName = SPA::Utilities::ToWide((const char*)colname, colnamelen / sizeof (SQLCHAR)); //original column name

                retcode = SQLColAttribute(hstmt, (SQLUSMALLINT) (n + 1), SQL_DESC_SCHEMA_NAME, colname, sizeof (colname), &colnamelen, &displaysize);
                assert(SQL_SUCCEEDED(retcode));
                if (colnamelen) {
                    info.TablePath= SPA::Utilities::ToWide((const char*)colname, colnamelen / sizeof (SQLCHAR));
                    info.TablePath += L".";
                }
                retcode = SQLColAttribute(hstmt, (SQLUSMALLINT) (n + 1), SQL_DESC_BASE_TABLE_NAME, colname, sizeof (colname), &colnamelen, &displaysize);
                assert(SQL_SUCCEEDED(retcode));
                info.TablePath += SPA::Utilities::ToWide((const char*)colname); //schema.table_name

                retcode = SQLColAttribute(hstmt, (SQLUSMALLINT) (n + 1), SQL_DESC_TYPE_NAME, colname, sizeof (colname), &colnamelen, &displaysize);
                assert(SQL_SUCCEEDED(retcode));
                info.DeclaredType = SPA::Utilities::ToWide((const char*)colname, colnamelen / sizeof (SQLCHAR)); //native data type

                retcode = SQLColAttribute(hstmt, (SQLUSMALLINT) (n + 1), SQL_DESC_CATALOG_NAME, colname, sizeof (colname), &colnamelen, &displaysize);
                assert(SQL_SUCCEEDED(retcode));
                info.DBPath = SPA::Utilities::ToWide((const char*)colname, colnamelen / sizeof (SQLCHAR)); //database name

                retcode = SQLColAttribute(hstmt, (SQLUSMALLINT) (n + 1), SQL_DESC_UNSIGNED, nullptr, 0, nullptr, &displaysize);
                assert(SQL_SUCCEEDED(retcode));
                switch (coltype) {
                    case SQL_CHAR:
                    case SQL_VARCHAR:
                    case SQL_LONGVARCHAR:
                        info.ColumnSize = (unsigned int) collen;
                        info.DataType = (VT_ARRAY | VT_I1);
                        retcode = SQLColAttribute(hstmt, (SQLUSMALLINT) (n + 1), SQL_DESC_CASE_SENSITIVE, nullptr, 0, nullptr, &displaysize);
                        assert(SQL_SUCCEEDED(retcode));
                        if (displaysize == SQL_TRUE) {
                            info.Flags |= CDBColumnInfo::FLAG_CASE_SENSITIVE;
                        }
                        break;
                    case SQL_WCHAR:
                    case SQL_WVARCHAR:
                    case SQL_WLONGVARCHAR:
                        info.ColumnSize = (unsigned int) collen;
                        info.DataType = VT_BSTR;
                        retcode = SQLColAttribute(hstmt, (SQLUSMALLINT) (n + 1), SQL_DESC_CASE_SENSITIVE, nullptr, 0, nullptr, &displaysize);
                        assert(SQL_SUCCEEDED(retcode));
                        if (displaysize == SQL_TRUE) {
                            info.Flags |= CDBColumnInfo::FLAG_CASE_SENSITIVE;
                        }
                        break;
                    case SQL_BINARY:
                    case SQL_VARBINARY:
                    case SQL_LONGVARBINARY:
                        info.ColumnSize = (unsigned int) collen;
                        info.DataType = (VT_ARRAY | VT_UI1);
                        break;
                    case SQL_DECIMAL:
                    case SQL_NUMERIC:
                        info.ColumnSize = coltype; //remember SQL data type
                        info.DataType = VT_DECIMAL;
                        info.Scale = (unsigned char) decimaldigits;
                        retcode = SQLColAttribute(hstmt, (SQLUSMALLINT) (n + 1), SQL_DESC_PRECISION, nullptr, 0, nullptr, &displaysize);
                        assert(SQL_SUCCEEDED(retcode));
                        info.Precision = (unsigned char) displaysize;
                        break;
                    case SQL_SMALLINT:
                        if (displaysize == SQL_TRUE) {
                            info.DataType = VT_UI2;
                        } else {
                            info.DataType = VT_I2;
                        }
                        break;
                    case SQL_INTEGER:
                        if (displaysize == SQL_TRUE) {
                            info.DataType = VT_UI4;
                        } else {
                            info.DataType = VT_I4;
                        }
                        break;
                    case SQL_REAL:
                        info.DataType = VT_R4;
                        break;
                    case SQL_FLOAT:
                    case SQL_DOUBLE:
                        info.ColumnSize = coltype; //remember SQL data type
                        info.DataType = VT_R8;
                        break;
                    case SQL_TINYINT:
                        if (displaysize == SQL_TRUE) {
                            info.DataType = VT_UI1;
                        } else {
                            info.DataType = VT_I1;
                        }
                        break;
                        break;
                    case SQL_BIGINT:
                        if (displaysize == SQL_TRUE) {
                            info.DataType = VT_UI8;
                        } else {
                            info.DataType = VT_I8;
                        }
                        break;
                    case SQL_BIT:
                        info.DataType = VT_BOOL;
                        info.Flags |= CDBColumnInfo::FLAG_IS_BIT;
                        break;
                    case SQL_GUID:
                        info.DataType = VT_CLSID;
                        break;
                    case SQL_TYPE_DATE:
                    case SQL_TYPE_TIME:
                    case SQL_TYPE_TIMESTAMP:
                        info.ColumnSize = coltype; //remember SQL data type
                        info.DataType = VT_DATE;
                        break;
                    case SQL_INTERVAL_MONTH:
                    case SQL_INTERVAL_YEAR:
                    case SQL_INTERVAL_YEAR_TO_MONTH:
                    case SQL_INTERVAL_DAY:
                    case SQL_INTERVAL_HOUR:
                    case SQL_INTERVAL_MINUTE:
                    case SQL_INTERVAL_SECOND:
                    case SQL_INTERVAL_DAY_TO_HOUR:
                    case SQL_INTERVAL_DAY_TO_MINUTE:
                    case SQL_INTERVAL_DAY_TO_SECOND:
                    case SQL_INTERVAL_HOUR_TO_MINUTE:
                    case SQL_INTERVAL_HOUR_TO_SECOND:
                    case SQL_INTERVAL_MINUTE_TO_SECOND:
                        info.ColumnSize = coltype; //remember SQL data type
                        info.Scale = (unsigned char) decimaldigits;
                        retcode = SQLColAttribute(hstmt, (SQLUSMALLINT) (n + 1), SQL_DESC_PRECISION, nullptr, 0, nullptr, &displaysize);
                        assert(SQL_SUCCEEDED(retcode));
                        info.Precision = (unsigned char) displaysize;
                        info.DataType = VT_DATE;
                        break;
                    default:
                        assert(false); //not supported
                        break;
                }

                retcode = SQLColAttribute(hstmt, (SQLUSMALLINT) (n + 1), SQL_DESC_AUTO_UNIQUE_VALUE, nullptr, 0, nullptr, &displaysize);
                assert(SQL_SUCCEEDED(retcode));
                if (displaysize == SQL_TRUE) {
                    info.Flags |= CDBColumnInfo::FLAG_AUTOINCREMENT;
                    info.Flags |= CDBColumnInfo::FLAG_UNIQUE;
                    info.Flags |= CDBColumnInfo::FLAG_PRIMARY_KEY;
                    info.Flags |= CDBColumnInfo::FLAG_NOT_NULL;
                    primary_key_set = true;
                }

                retcode = SQLColAttribute(hstmt, (SQLUSMALLINT) (n + 1), SQL_DESC_UPDATABLE, nullptr, 0, nullptr, &displaysize);
                assert(SQL_SUCCEEDED(retcode));
                if (displaysize == SQL_ATTR_READONLY) {
                    info.Flags |= CDBColumnInfo::FLAG_NOT_WRITABLE;
                }
            }

            if (meta && !primary_key_set) {

            }
            return vCols;
        }

        unsigned short COdbcImpl::ToSystemTime(const TIMESTAMP_STRUCT &d, SYSTEMTIME & st) {
            st.wYear = (unsigned short) d.year;
            st.wMonth = d.month;
            st.wDay = d.day;
            st.wHour = d.hour;
            st.wMinute = d.minute;
            st.wSecond = d.second;
            st.wMilliseconds = (unsigned short) (d.fraction / 1000000);
            return (unsigned short) ((d.fraction / 1000) % 1000);
        }

        void COdbcImpl::ToSystemTime(const TIME_STRUCT &d, SYSTEMTIME & st) {
            //start from 01/01/1900
            st.wYear = 1900;
            st.wMonth = 1;
            st.wDay = 1;
            st.wHour = d.hour;
            st.wMinute = d.minute;
            st.wSecond = d.second;
            st.wMilliseconds = 0;
        }

        void COdbcImpl::ToSystemTime(const DATE_STRUCT &d, SYSTEMTIME & st) {
            memset(&st, 0, sizeof (st));
            st.wYear = (unsigned short) d.year;
            st.wMonth = d.month;
            st.wDay = d.day;
        }

        void COdbcImpl::SetStringInfo(SQLHDBC hdbc, SQLUSMALLINT infoType, std::unordered_map<SQLUSMALLINT, CComVariant> &mapInfo) {
            SQLSMALLINT bufferLen = 0;
            SQLCHAR buffer[128] = {0};
            SQLRETURN retcode = SQLGetInfo(hdbc, infoType, buffer, (SQLSMALLINT)sizeof (buffer), &bufferLen);
            if (SQL_SUCCEEDED(retcode)) {
                mapInfo[infoType] = SPA::Utilities::ToWide((const char*) buffer).c_str();
            }
        }

        void COdbcImpl::SetUIntInfo(SQLHDBC hdbc, SQLUSMALLINT infoType, std::unordered_map<SQLUSMALLINT, CComVariant> &mapInfo) {
            SQLSMALLINT bufferLen = 0;
            unsigned int d = 0;
            SQLRETURN retcode = SQLGetInfo(hdbc, infoType, &d, (SQLSMALLINT)sizeof (d), &bufferLen);
            if (SQL_SUCCEEDED(retcode)) {
                mapInfo[infoType] = d;
            }
        }

        void COdbcImpl::SetIntInfo(SQLHDBC hdbc, SQLUSMALLINT infoType, std::unordered_map<SQLUSMALLINT, CComVariant> &mapInfo) {
            SQLSMALLINT bufferLen = 0;
            int d = 0;
            SQLRETURN retcode = SQLGetInfo(hdbc, infoType, &d, (SQLSMALLINT)sizeof (d), &bufferLen);
            if (SQL_SUCCEEDED(retcode)) {
                mapInfo[infoType] = d;
            }
        }

        void COdbcImpl::SetUInt64Info(SQLHDBC hdbc, SQLUSMALLINT infoType, std::unordered_map<SQLUSMALLINT, CComVariant> &mapInfo) {
            SQLSMALLINT bufferLen = 0;
            SQLULEN d = 0;
            SQLRETURN retcode = SQLGetInfo(hdbc, infoType, &d, (SQLSMALLINT)sizeof (d), &bufferLen);
            if (SQL_SUCCEEDED(retcode)) {
                mapInfo[infoType] = d;
            }
        }

        void COdbcImpl::SetUShortInfo(SQLHDBC hdbc, SQLUSMALLINT infoType, std::unordered_map<SQLUSMALLINT, CComVariant> &mapInfo) {
            SQLSMALLINT bufferLen = 0;
            SQLUSMALLINT d = 0;
            SQLRETURN retcode = SQLGetInfo(hdbc, infoType, &d, (SQLSMALLINT)sizeof (d), &bufferLen);
            if (SQL_SUCCEEDED(retcode)) {
                mapInfo[infoType] = d;
            }
        }

        bool COdbcImpl::PushInfo(SQLHDBC hdbc) {
            std::unordered_map<SQLUSMALLINT, CComVariant> mapInfo;

            SetStringInfo(hdbc, SQL_ACCESSIBLE_PROCEDURES, mapInfo);
            SetStringInfo(hdbc, SQL_ACCESSIBLE_TABLES, mapInfo);
            SetStringInfo(hdbc, SQL_DATABASE_NAME, mapInfo);
            SetStringInfo(hdbc, SQL_USER_NAME, mapInfo);
            SetStringInfo(hdbc, SQL_DATA_SOURCE_NAME, mapInfo);
            SetStringInfo(hdbc, SQL_DRIVER_NAME, mapInfo);
            SetStringInfo(hdbc, SQL_DRIVER_VER, mapInfo);
            SetStringInfo(hdbc, SQL_DRIVER_ODBC_VER, mapInfo);
            SetStringInfo(hdbc, SQL_ODBC_VER, mapInfo);
            SetStringInfo(hdbc, SQL_SERVER_NAME, mapInfo);
            SetStringInfo(hdbc, SQL_CATALOG_NAME, mapInfo);
            SetStringInfo(hdbc, SQL_CATALOG_TERM, mapInfo);
            SetStringInfo(hdbc, SQL_CATALOG_NAME_SEPARATOR, mapInfo);
            SetStringInfo(hdbc, SQL_COLLATION_SEQ, mapInfo);
            SetStringInfo(hdbc, SQL_COLUMN_ALIAS, mapInfo);
            SetStringInfo(hdbc, SQL_DATA_SOURCE_READ_ONLY, mapInfo);
            SetStringInfo(hdbc, SQL_DBMS_NAME, mapInfo);
            SetStringInfo(hdbc, SQL_DBMS_VER, mapInfo);
            SetStringInfo(hdbc, SQL_DESCRIBE_PARAMETER, mapInfo);
            SetStringInfo(hdbc, SQL_DM_VER, mapInfo);
            SetStringInfo(hdbc, SQL_EXPRESSIONS_IN_ORDERBY, mapInfo);
            SetStringInfo(hdbc, SQL_IDENTIFIER_QUOTE_CHAR, mapInfo);
            SetStringInfo(hdbc, SQL_INTEGRITY, mapInfo);
            SetStringInfo(hdbc, SQL_KEYWORDS, mapInfo);
            SetStringInfo(hdbc, SQL_LIKE_ESCAPE_CLAUSE, mapInfo);
            SetStringInfo(hdbc, SQL_MULT_RESULT_SETS, mapInfo);
            SetStringInfo(hdbc, SQL_MULTIPLE_ACTIVE_TXN, mapInfo);
            SetStringInfo(hdbc, SQL_NEED_LONG_DATA_LEN, mapInfo);
            SetStringInfo(hdbc, SQL_ORDER_BY_COLUMNS_IN_SELECT, mapInfo);
            SetStringInfo(hdbc, SQL_PROCEDURE_TERM, mapInfo);
            SetStringInfo(hdbc, SQL_PROCEDURES, mapInfo);
            SetStringInfo(hdbc, SQL_ROW_UPDATES, mapInfo);
            SetStringInfo(hdbc, SQL_SCHEMA_TERM, mapInfo);
            SetStringInfo(hdbc, SQL_SEARCH_PATTERN_ESCAPE, mapInfo);
            SetStringInfo(hdbc, SQL_SPECIAL_CHARACTERS, mapInfo);
            SetStringInfo(hdbc, SQL_SCHEMA_TERM, mapInfo);
            SetStringInfo(hdbc, SQL_TABLE_TERM, mapInfo);
            SetStringInfo(hdbc, SQL_XOPEN_CLI_YEAR, mapInfo);

            SetUShortInfo(hdbc, SQL_ACTIVE_ENVIRONMENTS, mapInfo);
            SetUShortInfo(hdbc, SQL_CATALOG_LOCATION, mapInfo);
            SetUShortInfo(hdbc, SQL_CONCAT_NULL_BEHAVIOR, mapInfo);
            SetUShortInfo(hdbc, SQL_CORRELATION_NAME, mapInfo);
            SetUShortInfo(hdbc, SQL_CURSOR_COMMIT_BEHAVIOR, mapInfo);
            SetUShortInfo(hdbc, SQL_CURSOR_ROLLBACK_BEHAVIOR, mapInfo);
            SetUShortInfo(hdbc, SQL_FILE_USAGE, mapInfo);
            SetUShortInfo(hdbc, SQL_GROUP_BY, mapInfo);
            SetUShortInfo(hdbc, SQL_IDENTIFIER_CASE, mapInfo);
            SetUShortInfo(hdbc, SQL_MAX_CATALOG_NAME_LEN, mapInfo);
            SetUShortInfo(hdbc, SQL_MAX_COLUMN_NAME_LEN, mapInfo);
            SetUShortInfo(hdbc, SQL_MAX_COLUMNS_IN_GROUP_BY, mapInfo);
            SetUShortInfo(hdbc, SQL_MAX_COLUMNS_IN_INDEX, mapInfo);
            SetUShortInfo(hdbc, SQL_MAX_COLUMNS_IN_ORDER_BY, mapInfo);
            SetUShortInfo(hdbc, SQL_MAX_COLUMNS_IN_SELECT, mapInfo);
            SetUShortInfo(hdbc, SQL_MAX_COLUMNS_IN_TABLE, mapInfo);
            SetUShortInfo(hdbc, SQL_MAX_CONCURRENT_ACTIVITIES, mapInfo);
            SetUShortInfo(hdbc, SQL_MAX_CURSOR_NAME_LEN, mapInfo);
            SetUShortInfo(hdbc, SQL_MAX_DRIVER_CONNECTIONS, mapInfo);
            SetUShortInfo(hdbc, SQL_MAX_IDENTIFIER_LEN, mapInfo);
            SetUShortInfo(hdbc, SQL_MAX_PROCEDURE_NAME_LEN, mapInfo);
            SetUShortInfo(hdbc, SQL_MAX_TABLE_NAME_LEN, mapInfo);
            SetUShortInfo(hdbc, SQL_MAX_TABLES_IN_SELECT, mapInfo);
            SetUShortInfo(hdbc, SQL_MAX_USER_NAME_LEN, mapInfo);
            SetUShortInfo(hdbc, SQL_NULL_COLLATION, mapInfo);
            SetUShortInfo(hdbc, SQL_QUOTED_IDENTIFIER_CASE, mapInfo);
            SetUShortInfo(hdbc, SQL_TXN_CAPABLE, mapInfo);

            SetIntInfo(hdbc, SQL_POS_OPERATIONS, mapInfo);

            SetUIntInfo(hdbc, SQL_AGGREGATE_FUNCTIONS, mapInfo);
            SetUIntInfo(hdbc, SQL_ALTER_DOMAIN, mapInfo);
            SetUIntInfo(hdbc, SQL_ALTER_TABLE, mapInfo);
            SetUIntInfo(hdbc, SQL_ASYNC_DBC_FUNCTIONS, mapInfo);
            SetUIntInfo(hdbc, SQL_ASYNC_MODE, mapInfo);
            SetUIntInfo(hdbc, SQL_BATCH_ROW_COUNT, mapInfo);
            SetUIntInfo(hdbc, SQL_BATCH_SUPPORT, mapInfo);
            SetUIntInfo(hdbc, SQL_CATALOG_USAGE, mapInfo);
            SetUIntInfo(hdbc, SQL_CONVERT_FUNCTIONS, mapInfo);
            SetUIntInfo(hdbc, SQL_CREATE_ASSERTION, mapInfo);
            SetUIntInfo(hdbc, SQL_CREATE_CHARACTER_SET, mapInfo);
            SetUIntInfo(hdbc, SQL_CREATE_COLLATION, mapInfo);
            SetUIntInfo(hdbc, SQL_CREATE_DOMAIN, mapInfo);
            SetUIntInfo(hdbc, SQL_CREATE_SCHEMA, mapInfo);
            SetUIntInfo(hdbc, SQL_CREATE_TABLE, mapInfo);
            SetUIntInfo(hdbc, SQL_CREATE_TRANSLATION, mapInfo);
            SetUIntInfo(hdbc, SQL_CREATE_VIEW, mapInfo);
            SetUIntInfo(hdbc, SQL_CURSOR_SENSITIVITY, mapInfo);
            SetUIntInfo(hdbc, SQL_DATETIME_LITERALS, mapInfo);
            SetUIntInfo(hdbc, SQL_DDL_INDEX, mapInfo);
            SetUIntInfo(hdbc, SQL_DEFAULT_TXN_ISOLATION, mapInfo);
            //SetUIntInfo(hdbc, SQL_DRIVER_AWARE_POOLING_SUPPORTED, mapInfo);
            SetUIntInfo(hdbc, SQL_DROP_ASSERTION, mapInfo);
            SetUIntInfo(hdbc, SQL_DROP_COLLATION, mapInfo);
            SetUIntInfo(hdbc, SQL_DROP_DOMAIN, mapInfo);
            SetUIntInfo(hdbc, SQL_DROP_SCHEMA, mapInfo);
            SetUIntInfo(hdbc, SQL_DROP_TABLE, mapInfo);
            SetUIntInfo(hdbc, SQL_DROP_TRANSLATION, mapInfo);
            SetUIntInfo(hdbc, SQL_DROP_VIEW, mapInfo);
            SetUIntInfo(hdbc, SQL_DYNAMIC_CURSOR_ATTRIBUTES1, mapInfo);
            SetUIntInfo(hdbc, SQL_DYNAMIC_CURSOR_ATTRIBUTES2, mapInfo);
            SetUIntInfo(hdbc, SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1, mapInfo);
            SetUIntInfo(hdbc, SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2, mapInfo);
            SetUIntInfo(hdbc, SQL_GETDATA_EXTENSIONS, mapInfo);
            SetUIntInfo(hdbc, SQL_INDEX_KEYWORDS, mapInfo);
            SetUIntInfo(hdbc, SQL_INFO_SCHEMA_VIEWS, mapInfo);
            SetUIntInfo(hdbc, SQL_INSERT_STATEMENT, mapInfo);
            SetUIntInfo(hdbc, SQL_KEYSET_CURSOR_ATTRIBUTES1, mapInfo);
            SetUIntInfo(hdbc, SQL_KEYSET_CURSOR_ATTRIBUTES2, mapInfo);
            SetUIntInfo(hdbc, SQL_MAX_ASYNC_CONCURRENT_STATEMENTS, mapInfo);
            SetUIntInfo(hdbc, SQL_MAX_BINARY_LITERAL_LEN, mapInfo);
            SetUIntInfo(hdbc, SQL_MAX_INDEX_SIZE, mapInfo);
            SetUIntInfo(hdbc, SQL_MAX_ROW_SIZE, mapInfo);
            SetUIntInfo(hdbc, SQL_MAX_STATEMENT_LEN, mapInfo);
            SetUIntInfo(hdbc, SQL_NUMERIC_FUNCTIONS, mapInfo);
            SetUIntInfo(hdbc, SQL_ODBC_INTERFACE_CONFORMANCE, mapInfo);
            SetUIntInfo(hdbc, SQL_OJ_CAPABILITIES, mapInfo);
            SetUIntInfo(hdbc, SQL_PARAM_ARRAY_SELECTS, mapInfo);
            SetUIntInfo(hdbc, SQL_SCHEMA_USAGE, mapInfo);
            SetUIntInfo(hdbc, SQL_SCROLL_OPTIONS, mapInfo);
            SetUIntInfo(hdbc, SQL_SQL_CONFORMANCE, mapInfo);
            SetUIntInfo(hdbc, SQL_SQL92_DATETIME_FUNCTIONS, mapInfo);
            SetUIntInfo(hdbc, SQL_SQL92_FOREIGN_KEY_DELETE_RULE, mapInfo);
            SetUIntInfo(hdbc, SQL_SQL92_FOREIGN_KEY_UPDATE_RULE, mapInfo);
            SetUIntInfo(hdbc, SQL_SQL92_GRANT, mapInfo);
            SetUIntInfo(hdbc, SQL_SQL92_NUMERIC_VALUE_FUNCTIONS, mapInfo);
            SetUIntInfo(hdbc, SQL_SQL92_PREDICATES, mapInfo);
            SetUIntInfo(hdbc, SQL_SQL92_RELATIONAL_JOIN_OPERATORS, mapInfo);
            SetUIntInfo(hdbc, SQL_SQL92_REVOKE, mapInfo);
            SetUIntInfo(hdbc, SQL_SQL92_ROW_VALUE_CONSTRUCTOR, mapInfo);
            SetUIntInfo(hdbc, SQL_SQL92_STRING_FUNCTIONS, mapInfo);
            SetUIntInfo(hdbc, SQL_SQL92_VALUE_EXPRESSIONS, mapInfo);
            SetUIntInfo(hdbc, SQL_STANDARD_CLI_CONFORMANCE, mapInfo);
            SetUIntInfo(hdbc, SQL_STATIC_CURSOR_ATTRIBUTES1, mapInfo);
            SetUIntInfo(hdbc, SQL_STATIC_CURSOR_ATTRIBUTES2, mapInfo);
            SetUIntInfo(hdbc, SQL_STRING_FUNCTIONS, mapInfo);
            SetUIntInfo(hdbc, SQL_SUBQUERIES, mapInfo);
            SetUIntInfo(hdbc, SQL_SYSTEM_FUNCTIONS, mapInfo);
            SetUIntInfo(hdbc, SQL_TIMEDATE_ADD_INTERVALS, mapInfo);
            SetUIntInfo(hdbc, SQL_TIMEDATE_DIFF_INTERVALS, mapInfo);
            SetUIntInfo(hdbc, SQL_TIMEDATE_FUNCTIONS, mapInfo);
            SetUIntInfo(hdbc, SQL_TXN_ISOLATION_OPTION, mapInfo);
            SetUIntInfo(hdbc, SQL_UNION, mapInfo);
            SetUIntInfo(hdbc, SQL_TXN_ISOLATION_OPTION, mapInfo);

            //SetUInt64Info(hdbc, SQL_DRIVER_HDBCSQL_DRIVER_HENV, mapInfo);
            SetUInt64Info(hdbc, SQL_DRIVER_HDESC, mapInfo);
            SetUInt64Info(hdbc, SQL_DRIVER_HLIB, mapInfo);
            SetUInt64Info(hdbc, SQL_DRIVER_HSTMT, mapInfo);

            CScopeUQueue sb;
            CUQueue &q = *sb;
            for (auto it = mapInfo.begin(), end = mapInfo.end(); it != end; ++it) {
                q << it->first << it->second;
            }
            unsigned int ret = SendResult(SPA::Odbc::idSQLGetInfo, q.GetBuffer(), q.GetSize());
            if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                return false;
            }
            return true;
        }

        void COdbcImpl::PreprocessPreparedStatement() {
            std::wstring s = m_sqlPrepare;
            m_bCall = false;
            m_procName.clear();
            if (s.size() && s.front() == L'{' && s.back() == L'}') {
                s.pop_back();
                s.erase(s.begin(), s.begin() + 1);
                COdbcImpl::ODBC_CONNECTION_STRING::Trim(s);
                transform(s.begin(), s.end(), s.begin(), ::tolower);
                m_bReturn = (s.front() == L'?');
                if (m_bReturn) {
                    s.erase(s.begin(), s.begin() + 1); //remove '?'
                    COdbcImpl::ODBC_CONNECTION_STRING::Trim(s);
                    if (s.front() != L'=')
                        return;
                    s.erase(s.begin(), s.begin() + 1); //remove '='
                    COdbcImpl::ODBC_CONNECTION_STRING::Trim(s);
                }
                m_bCall = (s.find(L"call ") == 0);
                if (m_bCall) {
                    auto pos = m_sqlPrepare.find('(');
                    if (pos != std::string::npos) {
                        m_procName.assign(m_sqlPrepare.begin() + 5, m_sqlPrepare.begin() + pos);
                    } else {
                        m_procName = m_sqlPrepare.substr(5);
                    }
                    COdbcImpl::ODBC_CONNECTION_STRING::Trim(m_procName);
                }
            }
        }

        bool COdbcImpl::PushRecords(SQLHSTMT hstmt, const CDBColumnInfoArray &vColInfo, int &res, std::wstring & errMsg) {
            SQLRETURN retcode;
            VARTYPE vt;
            CScopeUQueue sbTemp(MY_OPERATION_SYSTEM, SPA::IsBigEndian(), 2 * DEFAULT_BIG_FIELD_CHUNK_SIZE);
            SQLLEN len_or_null = 0;
            size_t fields = vColInfo.size();
            CScopeUQueue sb;
            CUQueue &q = *sb;
            while (true) {
                retcode = SQLFetch(hstmt);
                if (retcode == SQL_NO_DATA) {
                    break;
                }
                bool blob = false;
                if (SQL_SUCCEEDED(retcode)) {
                    for (size_t i = 0; i < fields; ++i) {
                        const CDBColumnInfo &colInfo = vColInfo[i];
                        vt = colInfo.DataType;
                        switch (vt) {
                            case VT_BOOL:
                            {
                                unsigned char boolean = 0;
                                retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_BIT, &boolean, sizeof (boolean), &len_or_null);
                                if (len_or_null == SQL_NULL_DATA) {
                                    q << (VARTYPE) VT_NULL;
                                } else {
                                    VARIANT_BOOL ok = boolean ? VARIANT_TRUE : VARIANT_FALSE;
                                    q << vt << ok;
                                }
                            }
                                break;
                            case VT_BSTR:
                                if (colInfo.ColumnSize >= DEFAULT_BIG_FIELD_CHUNK_SIZE) {
                                    if (!SendUText(hstmt, (SQLUSMALLINT) (i + 1), *sbTemp, q, blob)) {
                                        return false;
                                    }
                                } else {
#ifdef WIN32_64
                                    unsigned int max = (colInfo.ColumnSize << 1);
                                    if (q.GetTailSize() < sizeof (unsigned int) + sizeof (VARTYPE) + max + sizeof (wchar_t)) {
                                        q.ReallocBuffer(q.GetMaxSize() + max + sizeof (unsigned int) + sizeof (VARTYPE) + sizeof (wchar_t));
                                    }
                                    VARTYPE *pvt = (VARTYPE *) q.GetBuffer(q.GetSize());
                                    unsigned int *plen = (unsigned int*) (pvt + 1);
                                    unsigned char *pos = (unsigned char*) (plen + 1);
                                    retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_WCHAR, pos, q.GetTailSize() - sizeof (unsigned int) - sizeof (VARTYPE), &len_or_null);
                                    if (SQL_NULL_DATA == len_or_null) {
                                        q << (VARTYPE) VT_NULL;
                                    } else {
                                        *pvt = vt;
                                        *plen = (unsigned int) len_or_null;
                                        q.SetSize(q.GetSize() + *plen + sizeof (unsigned int) + sizeof (VARTYPE));
                                    }
#else
                                    unsigned int max = (colInfo.ColumnSize << 2 + sizeof (wchar_t));
                                    if (max > sbTemp->GetMaxSize()) {
                                        sbTemp->ReallocBuffer(max);
                                    }
                                    retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_WCHAR, (SQLPOINTER) sbTemp->GetBuffer(), sbTemp->GetMaxSize(), &len_or_null);
                                    if (SQL_NULL_DATA == len_or_null) {
                                        q << (VARTYPE) VT_NULL;
                                    } else {
                                        sbTemp->SetSize(len_or_null);
                                        sbTemp->SetNull();
                                        q << vt;
                                        q << (const wchar_t *) sbTemp->GetBuffer();
                                    }
                                    sbTemp->SetSize(0);
#endif
                                }
                                break;
                            case VT_DATE:
                                switch ((SQLSMALLINT) colInfo.ColumnSize) {
                                    case SQL_TYPE_DATE:
                                    {
                                        DATE_STRUCT d;
                                        retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_TYPE_DATE, &d, sizeof (d), &len_or_null);
                                        if (len_or_null == SQL_NULL_DATA) {
                                            q << (VARTYPE) VT_NULL;
                                        } else {
                                            q << vt;
                                            SYSTEMTIME st;
                                            ToSystemTime(d, st);
                                            SPA::UDateTime dt(st);
                                            q << dt.time;
                                        }
                                    }
                                        break;
                                    case SQL_TYPE_TIME:
                                    {
                                        TIME_STRUCT d;
                                        retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_TYPE_TIME, &d, sizeof (d), &len_or_null);
                                        if (len_or_null == SQL_NULL_DATA) {
                                            q << (VARTYPE) VT_NULL;
                                        } else {
                                            q << vt;
                                            SYSTEMTIME st;
                                            ToSystemTime(d, st);
                                            SPA::UDateTime dt(st);
                                            q << dt.time;
                                        }
                                    }
                                        break;
                                    case SQL_TYPE_TIMESTAMP:
                                    {
                                        TIMESTAMP_STRUCT d;
                                        retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_TYPE_TIMESTAMP, &d, sizeof (d), &len_or_null);
                                        if (len_or_null == SQL_NULL_DATA) {
                                            q << (VARTYPE) VT_NULL;
                                        } else {
                                            q << vt;
                                            SYSTEMTIME st;
                                            unsigned short us = ToSystemTime(d, st);
                                            SPA::UDateTime dt(st, us);
                                            q << dt.time;
                                        }
                                    }
                                        break;
                                    case SQL_INTERVAL_MONTH:
                                        break;
                                    case SQL_INTERVAL_YEAR:
                                        break;
                                    case SQL_INTERVAL_YEAR_TO_MONTH:
                                        break;
                                    case SQL_INTERVAL_DAY:
                                        break;
                                    case SQL_INTERVAL_HOUR:
                                        break;
                                    case SQL_INTERVAL_MINUTE:
                                        break;
                                    case SQL_INTERVAL_SECOND:
                                        break;
                                    case SQL_INTERVAL_DAY_TO_HOUR:
                                        break;
                                    case SQL_INTERVAL_DAY_TO_MINUTE:
                                        break;
                                    case SQL_INTERVAL_DAY_TO_SECOND:
                                        break;
                                    case SQL_INTERVAL_HOUR_TO_MINUTE:
                                        break;
                                    case SQL_INTERVAL_HOUR_TO_SECOND:
                                        break;
                                    case SQL_INTERVAL_MINUTE_TO_SECOND:
                                        break;
                                    default:
                                        assert(false); //shouldn't come here
                                        break;
                                }
                                break;
                            case VT_I1:
                            {
                                char d;
                                retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_TINYINT, &d, sizeof (d), &len_or_null);
                                if (len_or_null == SQL_NULL_DATA) {
                                    q << (VARTYPE) VT_NULL;
                                } else {
                                    q << vt;
                                    q.Push((const unsigned char*) &d, sizeof (d));
                                }
                            }
                                break;
                            case VT_UI1:
                            {
                                unsigned char d;
                                retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_UTINYINT, &d, sizeof (d), &len_or_null);
                                if (len_or_null == SQL_NULL_DATA) {
                                    q << (VARTYPE) VT_NULL;
                                } else {
                                    q << vt;
                                    q.Push((const unsigned char*) &d, sizeof (d));
                                }
                            }
                                break;
                            case VT_I2:
                            {
                                short d;
                                retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_SHORT, &d, sizeof (d), &len_or_null);
                                if (len_or_null == SQL_NULL_DATA) {
                                    q << (VARTYPE) VT_NULL;
                                } else {
                                    q << vt << d;
                                }
                            }
                                break;
                            case VT_UI2:
                            {
                                unsigned short d;
                                retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_USHORT, &d, sizeof (d), &len_or_null);
                                if (len_or_null == SQL_NULL_DATA) {
                                    q << (VARTYPE) VT_NULL;
                                } else {
                                    q << vt << d;
                                }
                            }
                                break;
                            case VT_I4:
                            {
                                int d;
                                retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_LONG, &d, sizeof (d), &len_or_null);
                                if (len_or_null == SQL_NULL_DATA) {
                                    q << (VARTYPE) VT_NULL;
                                } else {
                                    q << vt << d;
                                }
                            }
                                break;
                            case VT_UI4:
                            {
                                unsigned int d;
                                retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_ULONG, &d, sizeof (d), &len_or_null);
                                if (len_or_null == SQL_NULL_DATA) {
                                    q << (VARTYPE) VT_NULL;
                                } else {
                                    q << vt << d;
                                }
                            }
                                break;
                            case VT_R4:
                            {
                                float d;
                                retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_FLOAT, &d, sizeof (d), &len_or_null);
                                if (len_or_null == SQL_NULL_DATA) {
                                    q << (VARTYPE) VT_NULL;
                                } else {
                                    q << vt << d;
                                }
                            }
                                break;
                            case VT_I8:
                            {
                                SPA::INT64 d;
                                retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_SBIGINT, &d, sizeof (d), &len_or_null);
                                if (len_or_null == SQL_NULL_DATA) {
                                    q << (VARTYPE) VT_NULL;
                                } else {
                                    q << vt << d;
                                }
                            }
                                break;
                            case VT_UI8:
                            {
                                SPA::UINT64 d;
                                retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_UBIGINT, &d, sizeof (d), &len_or_null);
                                if (len_or_null == SQL_NULL_DATA) {
                                    q << (VARTYPE) VT_NULL;
                                } else {
                                    q << vt << d;
                                }
                            }
                                break;
                            case VT_R8:
                            {
                                double d;
                                retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_DOUBLE, &d, sizeof (d), &len_or_null);
                                if (len_or_null == SQL_NULL_DATA) {
                                    q << (VARTYPE) VT_NULL;
                                } else {
                                    q << vt << d;
                                }
                            }
                                break;
                            case (VT_ARRAY | VT_I1):
                                if (colInfo.ColumnSize < 2 * DEFAULT_BIG_FIELD_CHUNK_SIZE) {
                                    retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_CHAR, (SQLPOINTER) sbTemp->GetBuffer(), sbTemp->GetMaxSize(), &len_or_null);
                                    if (SQL_NULL_DATA == len_or_null) {
                                        q << (VARTYPE) VT_NULL;
                                    } else {
                                        q << vt << (unsigned int) len_or_null;
                                        q.Push(sbTemp->GetBuffer(), (unsigned int) len_or_null);
                                    }
                                } else {
                                    if (!SendBlob(hstmt, (SQLUSMALLINT) (i + 1), vt, *sbTemp, q, blob)) {
                                        return false;
                                    }
                                }
                                break;
                            case (VT_ARRAY | VT_UI1):
                                if (colInfo.Precision == sizeof (SQLGUID)) {
                                    retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_GUID, (SQLPOINTER) sbTemp->GetBuffer(), sbTemp->GetMaxSize(), &len_or_null);
                                    if (SQL_NULL_DATA == len_or_null) {
                                        q << (VARTYPE) VT_NULL;
                                    } else {
                                        q << vt;
                                        q.Push(sbTemp->GetBuffer(), sizeof (SQLGUID));
                                    }
                                } else if (colInfo.ColumnSize < 2 * DEFAULT_BIG_FIELD_CHUNK_SIZE) {
                                    retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_BINARY, (SQLPOINTER) sbTemp->GetBuffer(), sbTemp->GetMaxSize(), &len_or_null);
                                    if (SQL_NULL_DATA == len_or_null) {
                                        q << (VARTYPE) VT_NULL;
                                    } else {
                                        q << vt << (unsigned int) len_or_null;
                                        q.Push(sbTemp->GetBuffer(), (unsigned int) len_or_null);
                                    }
                                } else {
                                    if (!SendBlob(hstmt, (SQLUSMALLINT) (i + 1), vt, *sbTemp, q, blob)) {
                                        return false;
                                    }
                                }
                                break;
                            case VT_DECIMAL:
                                switch ((SQLSMALLINT) colInfo.ColumnSize) {
                                    case SQL_NUMERIC:
                                    case SQL_DECIMAL:
                                    {
                                        retcode = SQLGetData(hstmt, (SQLUSMALLINT) (i + 1), SQL_C_CHAR, (SQLPOINTER) sbTemp->GetBuffer(), sbTemp->GetMaxSize(), &len_or_null);
                                        if (len_or_null == SQL_NULL_DATA) {
                                            q << (VARTYPE) VT_NULL;
                                        } else {
                                            q << vt;
                                            sbTemp->SetSize((unsigned int) len_or_null);
                                            sbTemp->SetNull();
                                            DECIMAL dec;
                                            SPA::ParseDec((const char*) sbTemp->GetBuffer(), dec);
                                            q << dec;
                                            sbTemp->SetSize(0);
                                        }
                                    }
                                        break;
                                    default:
                                        assert(false); //shouldn't come here
                                        break;
                                }
                                break;
                            default:
                                assert(false);
                                break;
                        } //for loop
                        assert(SQL_SUCCEEDED(retcode));
                    }
                } else {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_STMT, hstmt, errMsg);
                    ++m_fails;
                    break;
                }
                if ((q.GetSize() >= DEFAULT_RECORD_BATCH_SIZE || blob) && !SendRows(q)) {
                    return false;
                }
            } //while loop
            assert(SQL_NO_DATA == retcode);
            if (q.GetSize()) {
                return SendRows(q);
            }
            return true;
        }

        void COdbcImpl::DoSQLColumnPrivileges(const std::wstring& catalogName, const std::wstring& schemaName, const std::wstring& tableName, const std::wstring& columnName, UINT64 index, int &res, std::wstring &errMsg, UINT64 & fail_ok) {
            fail_ok = 0;
            if (!m_pOdbc) {
                res = SPA::Odbc::ER_NO_DB_OPENED_YET;
                errMsg = NO_DB_OPENED_YET;
                ++m_fails;
                fail_ok = 1;
                fail_ok <<= 32;
                return;
            } else {
                res = 0;
            }
            UINT64 fails = m_fails;
            UINT64 oks = m_oks;
            SQLHSTMT hstmt = nullptr;
            SQLRETURN retcode = SQLAllocHandle(SQL_HANDLE_STMT, m_pOdbc.get(), &hstmt);
            do {
                std::shared_ptr<void> pStmt(hstmt, [](SQLHSTMT h) {
                    if (h) {
                        SQLRETURN ret = SQLFreeHandle(SQL_HANDLE_STMT, h);
                        assert(ret == SQL_SUCCESS);
                    }
                });
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_DBC, m_pOdbc.get(), errMsg);
                    ++m_fails;
                    break;
                }
                m_pExcuting = pStmt;
				std::string cn = SPA::Utilities::ToUTF8(catalogName.c_str(), catalogName.size());
				std::string sn = SPA::Utilities::ToUTF8(schemaName.c_str(), schemaName.size());
				std::string tn = SPA::Utilities::ToUTF8(tableName.c_str(), tableName.size());
				std::string coln = SPA::Utilities::ToUTF8(columnName.c_str(), columnName.size());
                retcode = SQLColumnPrivileges(hstmt, (SQLCHAR*) cn.c_str(), (SQLSMALLINT) cn.size(),
                        (SQLCHAR*) sn.c_str(), (SQLSMALLINT) sn.size(),
                        (SQLCHAR*) tn.c_str(), (SQLSMALLINT) tn.size(),
                        (SQLCHAR*) coln.c_str(), (SQLSMALLINT) coln.size());
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_STMT, hstmt, errMsg);
                    ++m_fails;
                    break;
                }
                unsigned int ret;
                SQLSMALLINT columns = 0;
                retcode = SQLNumResultCols(hstmt, &columns);
                assert(SQL_SUCCEEDED(retcode));
                CDBColumnInfoArray vInfo = GetColInfo(hstmt, columns, true);
                {
                    CScopeUQueue sbRowset;
                    sbRowset << vInfo << index;
                    ret = SendResult(idRowsetHeader, sbRowset->GetBuffer(), sbRowset->GetSize());
                }
                if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                    return;
                }
                bool ok = PushRecords(hstmt, vInfo, res, errMsg);
                ++m_oks;
                if (!ok) {
                    return;
                }
            } while (false);
            fail_ok = ((m_fails - fails) << 32);
            fail_ok += (unsigned int) (m_oks - oks);
        }

        void COdbcImpl::DoSQLTables(const std::wstring& catalogName, const std::wstring& schemaName, const std::wstring& tableName, const std::wstring& tableType, UINT64 index, int &res, std::wstring &errMsg, UINT64 & fail_ok) {
            fail_ok = 0;
            if (!m_pOdbc) {
                res = SPA::Odbc::ER_NO_DB_OPENED_YET;
                errMsg = NO_DB_OPENED_YET;
                ++m_fails;
                fail_ok = 1;
                fail_ok <<= 32;
                return;
            } else {
                res = 0;
            }
            UINT64 fails = m_fails;
            UINT64 oks = m_oks;
            SQLHSTMT hstmt = nullptr;
            SQLRETURN retcode = SQLAllocHandle(SQL_HANDLE_STMT, m_pOdbc.get(), &hstmt);
            do {
                std::shared_ptr<void> pStmt(hstmt, [](SQLHSTMT h) {
                    if (h) {
                        SQLRETURN ret = SQLFreeHandle(SQL_HANDLE_STMT, h);
                        assert(ret == SQL_SUCCESS);
                    }
                });
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_DBC, m_pOdbc.get(), errMsg);
                    ++m_fails;
                    break;
                }
                m_pExcuting = pStmt;
				std::string cn = SPA::Utilities::ToUTF8(catalogName.c_str(), catalogName.size());
				std::string sn = SPA::Utilities::ToUTF8(schemaName.c_str(), schemaName.size());
				std::string tn = SPA::Utilities::ToUTF8(tableName.c_str(), tableName.size());
				std::string tt = SPA::Utilities::ToUTF8(tableType.c_str(), tableType.size());
                retcode = SQLTables(hstmt, (SQLCHAR*) cn.c_str(), (SQLSMALLINT) cn.size(),
                        (SQLCHAR*) sn.c_str(), (SQLSMALLINT) sn.size(),
                        (SQLCHAR*) tn.c_str(), (SQLSMALLINT) tn.size(),
                        (SQLCHAR*) tt.c_str(), (SQLSMALLINT) tt.size());
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_STMT, hstmt, errMsg);
                    ++m_fails;
                    break;
                }
                unsigned int ret;
                SQLSMALLINT columns = 0;
                retcode = SQLNumResultCols(hstmt, &columns);
                assert(SQL_SUCCEEDED(retcode));
                CDBColumnInfoArray vInfo = GetColInfo(hstmt, columns, true);
                {
                    CScopeUQueue sbRowset;
                    sbRowset << vInfo << index;
                    ret = SendResult(idRowsetHeader, sbRowset->GetBuffer(), sbRowset->GetSize());
                }
                if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                    return;
                }
                bool ok = PushRecords(hstmt, vInfo, res, errMsg);
                ++m_oks;
                if (!ok) {
                    return;
                }
            } while (false);
            fail_ok = ((m_fails - fails) << 32);
            fail_ok += (unsigned int) (m_oks - oks);
        }

        void COdbcImpl::DoSQLColumns(const std::wstring& catalogName, const std::wstring& schemaName, const std::wstring& tableName, const std::wstring& columnName, UINT64 index, int &res, std::wstring &errMsg, UINT64 & fail_ok) {
            fail_ok = 0;
            if (!m_pOdbc) {
                res = SPA::Odbc::ER_NO_DB_OPENED_YET;
                errMsg = NO_DB_OPENED_YET;
                ++m_fails;
                fail_ok = 1;
                fail_ok <<= 32;
                return;
            } else {
                res = 0;
            }
            UINT64 fails = m_fails;
            UINT64 oks = m_oks;
            SQLHSTMT hstmt = nullptr;
            SQLRETURN retcode = SQLAllocHandle(SQL_HANDLE_STMT, m_pOdbc.get(), &hstmt);
            do {
                std::shared_ptr<void> pStmt(hstmt, [](SQLHSTMT h) {
                    if (h) {
                        SQLRETURN ret = SQLFreeHandle(SQL_HANDLE_STMT, h);
                        assert(ret == SQL_SUCCESS);
                    }
                });
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_DBC, m_pOdbc.get(), errMsg);
                    ++m_fails;
                    break;
                }
                m_pExcuting = pStmt;
				std::string cn = SPA::Utilities::ToUTF8(catalogName.c_str(), catalogName.size());
				std::string sn = SPA::Utilities::ToUTF8(schemaName.c_str(), schemaName.size());
				std::string tn = SPA::Utilities::ToUTF8(tableName.c_str(), tableName.size());
				std::string coln = SPA::Utilities::ToUTF8(columnName.c_str(), columnName.size());
                retcode = SQLColumns(hstmt, (SQLCHAR*) cn.c_str(), (SQLSMALLINT) cn.size(),
                        (SQLCHAR*) sn.c_str(), (SQLSMALLINT) sn.size(),
                        (SQLCHAR*) tn.c_str(), (SQLSMALLINT) tn.size(),
                        (SQLCHAR*) coln.c_str(), (SQLSMALLINT) coln.size());
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_STMT, hstmt, errMsg);
                    ++m_fails;
                    break;
                }
                unsigned int ret;
                SQLSMALLINT columns = 0;
                retcode = SQLNumResultCols(hstmt, &columns);
                assert(SQL_SUCCEEDED(retcode));
                CDBColumnInfoArray vInfo = GetColInfo(hstmt, columns, true);
                {
                    CScopeUQueue sbRowset;
                    sbRowset << vInfo << index;
                    ret = SendResult(idRowsetHeader, sbRowset->GetBuffer(), sbRowset->GetSize());
                }
                if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                    return;
                }
                bool ok = PushRecords(hstmt, vInfo, res, errMsg);
                ++m_oks;
                if (!ok) {
                    return;
                }
            } while (false);
            fail_ok = ((m_fails - fails) << 32);
            fail_ok += (unsigned int) (m_oks - oks);
        }

        void COdbcImpl::DoSQLProcedureColumns(const std::wstring& catalogName, const std::wstring& schemaName, const std::wstring& procName, const std::wstring& columnName, UINT64 index, int &res, std::wstring &errMsg, UINT64 & fail_ok) {
            fail_ok = 0;
            if (!m_pOdbc) {
                res = SPA::Odbc::ER_NO_DB_OPENED_YET;
                errMsg = NO_DB_OPENED_YET;
                ++m_fails;
                fail_ok = 1;
                fail_ok <<= 32;
                return;
            } else {
                res = 0;
            }
            UINT64 fails = m_fails;
            UINT64 oks = m_oks;
            SQLHSTMT hstmt = nullptr;
            SQLRETURN retcode = SQLAllocHandle(SQL_HANDLE_STMT, m_pOdbc.get(), &hstmt);
            do {
                std::shared_ptr<void> pStmt(hstmt, [](SQLHSTMT h) {
                    if (h) {
                        SQLRETURN ret = SQLFreeHandle(SQL_HANDLE_STMT, h);
                        assert(ret == SQL_SUCCESS);
                    }
                });
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_DBC, m_pOdbc.get(), errMsg);
                    ++m_fails;
                    break;
                }
                m_pExcuting = pStmt;
				std::string cn = SPA::Utilities::ToUTF8(catalogName.c_str(), catalogName.size());
				std::string sn = SPA::Utilities::ToUTF8(schemaName.c_str(), schemaName.size());
				std::string pn = SPA::Utilities::ToUTF8(procName.c_str(), procName.size());
				std::string coln = SPA::Utilities::ToUTF8(columnName.c_str(), columnName.size());
                retcode = SQLProcedureColumns(hstmt, (SQLCHAR*) cn.c_str(), (SQLSMALLINT) cn.size(),
                        (SQLCHAR*) sn.c_str(), (SQLSMALLINT) sn.size(),
                        (SQLCHAR*) pn.c_str(), (SQLSMALLINT) pn.size(),
                        (SQLCHAR*) coln.c_str(), (SQLSMALLINT) coln.size());
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_STMT, hstmt, errMsg);
                    ++m_fails;
                    break;
                }
                unsigned int ret;
                SQLSMALLINT columns = 0;
                retcode = SQLNumResultCols(hstmt, &columns);
                assert(SQL_SUCCEEDED(retcode));
                CDBColumnInfoArray vInfo = GetColInfo(hstmt, columns, true);
                {
                    CScopeUQueue sbRowset;
                    sbRowset << vInfo << index;
                    ret = SendResult(idRowsetHeader, sbRowset->GetBuffer(), sbRowset->GetSize());
                }
                if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                    return;
                }
                bool ok = PushRecords(hstmt, vInfo, res, errMsg);
                ++m_oks;
                if (!ok) {
                    return;
                }
            } while (false);
            fail_ok = ((m_fails - fails) << 32);
            fail_ok += (unsigned int) (m_oks - oks);
        }

        void COdbcImpl::DoSQLPrimaryKeys(const std::wstring& catalogName, const std::wstring& schemaName, const std::wstring& tableName, UINT64 index, int &res, std::wstring &errMsg, UINT64 & fail_ok) {
            fail_ok = 0;
            if (!m_pOdbc) {
                res = SPA::Odbc::ER_NO_DB_OPENED_YET;
                errMsg = NO_DB_OPENED_YET;
                ++m_fails;
                fail_ok = 1;
                fail_ok <<= 32;
                return;
            } else {
                res = 0;
            }
            UINT64 fails = m_fails;
            UINT64 oks = m_oks;
            SQLHSTMT hstmt = nullptr;
            SQLRETURN retcode = SQLAllocHandle(SQL_HANDLE_STMT, m_pOdbc.get(), &hstmt);
            do {
                std::shared_ptr<void> pStmt(hstmt, [](SQLHSTMT h) {
                    if (h) {
                        SQLRETURN ret = SQLFreeHandle(SQL_HANDLE_STMT, h);
                        assert(ret == SQL_SUCCESS);
                    }
                });
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_DBC, m_pOdbc.get(), errMsg);
                    ++m_fails;
                    break;
                }
                m_pExcuting = pStmt;
				std::string cn = SPA::Utilities::ToUTF8(catalogName.c_str(), catalogName.size());
				std::string sn = SPA::Utilities::ToUTF8(schemaName.c_str(), schemaName.size());
				std::string tn = SPA::Utilities::ToUTF8(tableName.c_str(), tableName.size());
                retcode = SQLPrimaryKeys(hstmt, (SQLCHAR*) cn.c_str(), (SQLSMALLINT) cn.size(),
                        (SQLCHAR*) sn.c_str(), (SQLSMALLINT) sn.size(),
                        (SQLCHAR*) tn.c_str(), (SQLSMALLINT) tn.size());
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_STMT, hstmt, errMsg);
                    ++m_fails;
                    break;
                }
                unsigned int ret;
                SQLSMALLINT columns = 0;
                retcode = SQLNumResultCols(hstmt, &columns);
                assert(SQL_SUCCEEDED(retcode));
                CDBColumnInfoArray vInfo = GetColInfo(hstmt, columns, true);
                {
                    CScopeUQueue sbRowset;
                    sbRowset << vInfo << index;
                    ret = SendResult(idRowsetHeader, sbRowset->GetBuffer(), sbRowset->GetSize());
                }
                if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                    return;
                }
                bool ok = PushRecords(hstmt, vInfo, res, errMsg);
                ++m_oks;
                if (!ok) {
                    return;
                }
            } while (false);
            fail_ok = ((m_fails - fails) << 32);
            fail_ok += (unsigned int) (m_oks - oks);
        }

        void COdbcImpl::DoSQLTablePrivileges(const std::wstring& catalogName, const std::wstring& schemaName, const std::wstring& tableName, UINT64 index, int &res, std::wstring &errMsg, UINT64 & fail_ok) {
            fail_ok = 0;
            if (!m_pOdbc) {
                res = SPA::Odbc::ER_NO_DB_OPENED_YET;
                errMsg = NO_DB_OPENED_YET;
                ++m_fails;
                fail_ok = 1;
                fail_ok <<= 32;
                return;
            } else {
                res = 0;
            }
            UINT64 fails = m_fails;
            UINT64 oks = m_oks;
            SQLHSTMT hstmt = nullptr;
            SQLRETURN retcode = SQLAllocHandle(SQL_HANDLE_STMT, m_pOdbc.get(), &hstmt);
            do {
                std::shared_ptr<void> pStmt(hstmt, [](SQLHSTMT h) {
                    if (h) {
                        SQLRETURN ret = SQLFreeHandle(SQL_HANDLE_STMT, h);
                        assert(ret == SQL_SUCCESS);
                    }
                });
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_DBC, m_pOdbc.get(), errMsg);
                    ++m_fails;
                    break;
                }
                m_pExcuting = pStmt;
				std::string cn = SPA::Utilities::ToUTF8(catalogName.c_str(), catalogName.size());
				std::string sn = SPA::Utilities::ToUTF8(schemaName.c_str(), schemaName.size());
				std::string tn = SPA::Utilities::ToUTF8(tableName.c_str(), tableName.size());
                retcode = SQLTablePrivileges(hstmt, (SQLCHAR*) cn.c_str(), (SQLSMALLINT) cn.size(),
                        (SQLCHAR*) sn.c_str(), (SQLSMALLINT) sn.size(),
                        (SQLCHAR*) tn.c_str(), (SQLSMALLINT) tn.size());
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_STMT, hstmt, errMsg);
                    ++m_fails;
                    break;
                }
                unsigned int ret;
                SQLSMALLINT columns = 0;
                retcode = SQLNumResultCols(hstmt, &columns);
                assert(SQL_SUCCEEDED(retcode));
                CDBColumnInfoArray vInfo = GetColInfo(hstmt, columns, true);
                {
                    CScopeUQueue sbRowset;
                    sbRowset << vInfo << index;
                    ret = SendResult(idRowsetHeader, sbRowset->GetBuffer(), sbRowset->GetSize());
                }
                if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                    return;
                }
                bool ok = PushRecords(hstmt, vInfo, res, errMsg);
                ++m_oks;
                if (!ok) {
                    return;
                }
            } while (false);
            fail_ok = ((m_fails - fails) << 32);
            fail_ok += (unsigned int) (m_oks - oks);
        }

        void COdbcImpl::DoSQLStatistics(const std::wstring& catalogName, const std::wstring& schemaName, const std::wstring& tableName, SQLUSMALLINT unique, SQLUSMALLINT reserved, UINT64 index, int &res, std::wstring &errMsg, UINT64 & fail_ok) {
            fail_ok = 0;
            if (!m_pOdbc) {
                res = SPA::Odbc::ER_NO_DB_OPENED_YET;
                errMsg = NO_DB_OPENED_YET;
                ++m_fails;
                fail_ok = 1;
                fail_ok <<= 32;
                return;
            } else {
                res = 0;
            }
            UINT64 fails = m_fails;
            UINT64 oks = m_oks;
            SQLHSTMT hstmt = nullptr;
            SQLRETURN retcode = SQLAllocHandle(SQL_HANDLE_STMT, m_pOdbc.get(), &hstmt);
            do {
                std::shared_ptr<void> pStmt(hstmt, [](SQLHSTMT h) {
                    if (h) {
                        SQLRETURN ret = SQLFreeHandle(SQL_HANDLE_STMT, h);
                        assert(ret == SQL_SUCCESS);
                    }
                });
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_DBC, m_pOdbc.get(), errMsg);
                    ++m_fails;
                    break;
                }
                m_pExcuting = pStmt;
				std::string cn = SPA::Utilities::ToUTF8(catalogName.c_str(), catalogName.size());
				std::string sn = SPA::Utilities::ToUTF8(schemaName.c_str(), schemaName.size());
				std::string tn = SPA::Utilities::ToUTF8(tableName.c_str(), tableName.size());
                retcode = SQLStatistics(hstmt, (SQLCHAR*) cn.c_str(), (SQLSMALLINT) cn.size(),
                        (SQLCHAR*) sn.c_str(), (SQLSMALLINT) sn.size(),
                        (SQLCHAR*) tn.c_str(), (SQLSMALLINT) tn.size(), unique, reserved);
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_STMT, hstmt, errMsg);
                    ++m_fails;
                    break;
                }
                unsigned int ret;
                SQLSMALLINT columns = 0;
                retcode = SQLNumResultCols(hstmt, &columns);
                assert(SQL_SUCCEEDED(retcode));
                CDBColumnInfoArray vInfo = GetColInfo(hstmt, columns, true);
                {
                    CScopeUQueue sbRowset;
                    sbRowset << vInfo << index;
                    ret = SendResult(idRowsetHeader, sbRowset->GetBuffer(), sbRowset->GetSize());
                }
                if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                    return;
                }
                bool ok = PushRecords(hstmt, vInfo, res, errMsg);
                ++m_oks;
                if (!ok) {
                    return;
                }
            } while (false);
            fail_ok = ((m_fails - fails) << 32);
            fail_ok += (unsigned int) (m_oks - oks);
        }

        void COdbcImpl::DoSQLProcedures(const std::wstring& catalogName, const std::wstring& schemaName, const std::wstring& procName, UINT64 index, int &res, std::wstring &errMsg, UINT64 & fail_ok) {
            fail_ok = 0;
            if (!m_pOdbc) {
                res = SPA::Odbc::ER_NO_DB_OPENED_YET;
                errMsg = NO_DB_OPENED_YET;
                ++m_fails;
                fail_ok = 1;
                fail_ok <<= 32;
                return;
            } else {
                res = 0;
            }
            UINT64 fails = m_fails;
            UINT64 oks = m_oks;
            SQLHSTMT hstmt = nullptr;
            SQLRETURN retcode = SQLAllocHandle(SQL_HANDLE_STMT, m_pOdbc.get(), &hstmt);
            do {
                std::shared_ptr<void> pStmt(hstmt, [](SQLHSTMT h) {
                    if (h) {
                        SQLRETURN ret = SQLFreeHandle(SQL_HANDLE_STMT, h);
                        assert(ret == SQL_SUCCESS);
                    }
                });
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_DBC, m_pOdbc.get(), errMsg);
                    ++m_fails;
                    break;
                }
                m_pExcuting = pStmt;
				std::string cn = SPA::Utilities::ToUTF8(catalogName.c_str(), catalogName.size());
				std::string sn = SPA::Utilities::ToUTF8(schemaName.c_str(), schemaName.size());
				std::string pn = SPA::Utilities::ToUTF8(procName.c_str(), procName.size());
                retcode = SQLProcedures(hstmt, (SQLCHAR*) cn.c_str(), (SQLSMALLINT) cn.size(),
                        (SQLCHAR*) sn.c_str(), (SQLSMALLINT) sn.size(),
                        (SQLCHAR*) pn.c_str(), (SQLSMALLINT) pn.size());
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_STMT, hstmt, errMsg);
                    ++m_fails;
                    break;
                }
                unsigned int ret;
                SQLSMALLINT columns = 0;
                retcode = SQLNumResultCols(hstmt, &columns);
                assert(SQL_SUCCEEDED(retcode));
                CDBColumnInfoArray vInfo = GetColInfo(hstmt, columns, true);
                {
                    CScopeUQueue sbRowset;
                    sbRowset << vInfo << index;
                    ret = SendResult(idRowsetHeader, sbRowset->GetBuffer(), sbRowset->GetSize());
                }
                if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                    return;
                }
                bool ok = PushRecords(hstmt, vInfo, res, errMsg);
                ++m_oks;
                if (!ok) {
                    return;
                }
            } while (false);
            fail_ok = ((m_fails - fails) << 32);
            fail_ok += (unsigned int) (m_oks - oks);
        }

        void COdbcImpl::DoSQLSpecialColumns(SQLSMALLINT identifierType, const std::wstring& catalogName, const std::wstring& schemaName, const std::wstring& tableName, SQLSMALLINT scope, SQLSMALLINT nullable, UINT64 index, int &res, std::wstring &errMsg, UINT64 & fail_ok) {
            fail_ok = 0;
            if (!m_pOdbc) {
                res = SPA::Odbc::ER_NO_DB_OPENED_YET;
                errMsg = NO_DB_OPENED_YET;
                ++m_fails;
                fail_ok = 1;
                fail_ok <<= 32;
                return;
            } else {
                res = 0;
            }
            UINT64 fails = m_fails;
            UINT64 oks = m_oks;
            SQLHSTMT hstmt = nullptr;
            SQLRETURN retcode = SQLAllocHandle(SQL_HANDLE_STMT, m_pOdbc.get(), &hstmt);
            do {
                std::shared_ptr<void> pStmt(hstmt, [](SQLHSTMT h) {
                    if (h) {
                        SQLRETURN ret = SQLFreeHandle(SQL_HANDLE_STMT, h);
                        assert(ret == SQL_SUCCESS);
                    }
                });
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_DBC, m_pOdbc.get(), errMsg);
                    ++m_fails;
                    break;
                }
                m_pExcuting = pStmt;
				std::string cn = SPA::Utilities::ToUTF8(catalogName.c_str(), catalogName.size());
				std::string sn = SPA::Utilities::ToUTF8(schemaName.c_str(), schemaName.size());
				std::string tn = SPA::Utilities::ToUTF8(tableName.c_str(), tableName.size());
                retcode = SQLSpecialColumns(hstmt, identifierType, (SQLCHAR*) cn.c_str(), (SQLSMALLINT) cn.size(),
                        (SQLCHAR*) sn.c_str(), (SQLSMALLINT) sn.size(),
                        (SQLCHAR*) tn.c_str(), (SQLSMALLINT) tn.size(), scope, nullable);
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_STMT, hstmt, errMsg);
                    ++m_fails;
                    break;
                }
                unsigned int ret;
                SQLSMALLINT columns = 0;
                retcode = SQLNumResultCols(hstmt, &columns);
                assert(SQL_SUCCEEDED(retcode));
                CDBColumnInfoArray vInfo = GetColInfo(hstmt, columns, true);
                {
                    CScopeUQueue sbRowset;
                    sbRowset << vInfo << index;
                    ret = SendResult(idRowsetHeader, sbRowset->GetBuffer(), sbRowset->GetSize());
                }
                if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                    return;
                }
                bool ok = PushRecords(hstmt, vInfo, res, errMsg);
                ++m_oks;
                if (!ok) {
                    return;
                }
            } while (false);
            fail_ok = ((m_fails - fails) << 32);
            fail_ok += (unsigned int) (m_oks - oks);
        }

        void COdbcImpl::DoSQLForeignKeys(const std::wstring& pkCatalogName, const std::wstring& pkSchemaName, const std::wstring& pkTableName, const std::wstring& fkCatalogName, const std::wstring& fkSchemaName, const std::wstring& fkTableName, UINT64 index, int &res, std::wstring &errMsg, UINT64 & fail_ok) {
            fail_ok = 0;
            if (!m_pOdbc) {
                res = SPA::Odbc::ER_NO_DB_OPENED_YET;
                errMsg = NO_DB_OPENED_YET;
                ++m_fails;
                fail_ok = 1;
                fail_ok <<= 32;
                return;
            } else {
                res = 0;
            }
            UINT64 fails = m_fails;
            UINT64 oks = m_oks;
            SQLHSTMT hstmt = nullptr;
            SQLRETURN retcode = SQLAllocHandle(SQL_HANDLE_STMT, m_pOdbc.get(), &hstmt);
            do {
                std::shared_ptr<void> pStmt(hstmt, [](SQLHSTMT h) {
                    if (h) {
                        SQLRETURN ret = SQLFreeHandle(SQL_HANDLE_STMT, h);
                        assert(ret == SQL_SUCCESS);
                    }
                });
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_DBC, m_pOdbc.get(), errMsg);
                    ++m_fails;
                    break;
                }
                m_pExcuting = pStmt;
				std::string pk_cn = SPA::Utilities::ToUTF8(pkCatalogName.c_str(), pkCatalogName.size());
				std::string pk_sn = SPA::Utilities::ToUTF8(pkSchemaName.c_str(), pkSchemaName.size());
				std::string pk_tn = SPA::Utilities::ToUTF8(pkTableName.c_str(), pkTableName.size());
				std::string fk_cn = SPA::Utilities::ToUTF8(fkCatalogName.c_str(), fkCatalogName.size());
				std::string fk_sn = SPA::Utilities::ToUTF8(fkSchemaName.c_str(), fkSchemaName.size());
				std::string fk_tn = SPA::Utilities::ToUTF8(fkTableName.c_str(), fkTableName.size());
                retcode = SQLForeignKeys(hstmt, (SQLCHAR*) pk_cn.c_str(), (SQLSMALLINT) pk_cn.size(),
                        (SQLCHAR*) pkSchemaName.c_str(), (SQLSMALLINT) pkSchemaName.size(),
                        (SQLCHAR*) pk_tn.c_str(), (SQLSMALLINT) pk_tn.size(),
                        (SQLCHAR*) fk_cn.c_str(), (SQLSMALLINT) fk_cn.size(),
                        (SQLCHAR*) fk_sn.c_str(), (SQLSMALLINT) fk_sn.size(),
                        (SQLCHAR*) fk_sn.c_str(), (SQLSMALLINT) fk_tn.size());
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_STMT, hstmt, errMsg);
                    ++m_fails;
                    break;
                }
                unsigned int ret;
                SQLSMALLINT columns = 0;
                retcode = SQLNumResultCols(hstmt, &columns);
                assert(SQL_SUCCEEDED(retcode));
                CDBColumnInfoArray vInfo = GetColInfo(hstmt, columns, true);
                {
                    CScopeUQueue sbRowset;
                    sbRowset << vInfo << index;
                    ret = SendResult(idRowsetHeader, sbRowset->GetBuffer(), sbRowset->GetSize());
                }
                if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                    return;
                }
                bool ok = PushRecords(hstmt, vInfo, res, errMsg);
                ++m_oks;
                if (!ok) {
                    return;
                }
            } while (false);
            fail_ok = ((m_fails - fails) << 32);
            fail_ok += (unsigned int) (m_oks - oks);
        }

        void COdbcImpl::Execute(const std::wstring& wsql, bool rowset, bool meta, bool lastInsertId, UINT64 index, INT64 &affected, int &res, std::wstring &errMsg, CDBVariant &vtId, UINT64 & fail_ok) {
            affected = 0;
            fail_ok = 0;
            if (!m_pOdbc) {
                res = SPA::Odbc::ER_NO_DB_OPENED_YET;
                errMsg = NO_DB_OPENED_YET;
                ++m_fails;
                fail_ok = 1;
                fail_ok <<= 32;
                return;
            } else {
                res = 0;
            }
            UINT64 fails = m_fails;
            UINT64 oks = m_oks;
            SQLHSTMT hstmt = nullptr;
            SQLRETURN retcode = SQLAllocHandle(SQL_HANDLE_STMT, m_pOdbc.get(), &hstmt);
            do {
                std::shared_ptr<void> pStmt(hstmt, [](SQLHSTMT h) {
                    if (h) {
                        SQLRETURN ret = SQLFreeHandle(SQL_HANDLE_STMT, h);
                        assert(ret == SQL_SUCCESS);
                    }
                });
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_DBC, m_pOdbc.get(), errMsg);
                    ++m_fails;
                    break;
                }
                m_pExcuting = pStmt;
				std::string sql = SPA::Utilities::ToUTF8(wsql.c_str(), wsql.size());
                retcode = SQLExecDirect(hstmt, (SQLCHAR*) sql.c_str(), (SQLINTEGER) sql.size());
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_STMT, hstmt, errMsg);
                    ++m_fails;
                    break;
                }
                do {
                    SQLSMALLINT columns = 0;
                    retcode = SQLNumResultCols(hstmt, &columns);
                    assert(SQL_SUCCEEDED(retcode));
                    if (columns > 0) {
                        if (rowset) {
                            unsigned int ret;
                            CDBColumnInfoArray vInfo = GetColInfo(hstmt, columns, meta);
                            {
                                CScopeUQueue sbRowset;
                                sbRowset << vInfo << index;
                                ret = SendResult(idRowsetHeader, sbRowset->GetBuffer(), sbRowset->GetSize());
                            }
                            if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                                return;
                            }
                            bool ok = PushRecords(hstmt, vInfo, res, errMsg);
                            ++m_oks;
                            if (!ok) {
                                return;
                            }
                        }
                    } else {
                        SQLLEN rows = 0;
                        retcode = SQLRowCount(hstmt, &rows);
                        assert(SQL_SUCCEEDED(retcode));
                        if (rows > 0) {
                            affected += rows;
                        }
                        ++m_oks;
                    }
                } while ((retcode = SQLMoreResults(hstmt)) == SQL_SUCCESS);
                assert(retcode == SQL_NO_DATA);
            } while (false);
            fail_ok = ((m_fails - fails) << 32);
            fail_ok += (unsigned int) (m_oks - oks);
        }

        void COdbcImpl::Prepare(const std::wstring& wsql, CParameterInfoArray& params, int &res, std::wstring &errMsg, unsigned int &parameters) {
            m_vPInfo = params;
            parameters = 0;
            if (!m_pOdbc) {
                res = SPA::Odbc::ER_NO_DB_OPENED_YET;
                errMsg = NO_DB_OPENED_YET;
                return;
            } else {
                res = 0;
            }
            m_pPrepare.reset();
            m_vParam.clear();
            m_sqlPrepare = wsql;
            COdbcImpl::ODBC_CONNECTION_STRING::Trim(m_sqlPrepare);
            PreprocessPreparedStatement();
            SQLHSTMT hstmt = nullptr;
            SQLRETURN retcode = SQLAllocHandle(SQL_HANDLE_STMT, m_pOdbc.get(), &hstmt);
            do {
                m_pPrepare.reset(hstmt, [](SQLHSTMT h) {
                    if (h) {
                        SQLRETURN ret = SQLFreeHandle(SQL_HANDLE_STMT, h);
                        assert(ret == SQL_SUCCESS);
                    }
                });
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_DBC, m_pOdbc.get(), errMsg);
                    break;
                }
                m_pExcuting = m_pPrepare;
				std::string prepare = SPA::Utilities::ToUTF8(m_sqlPrepare.c_str(), m_sqlPrepare.size());
                retcode = SQLPrepare(hstmt, (SQLCHAR*) prepare.c_str(), (SQLINTEGER) m_sqlPrepare.size());
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_STMT, hstmt, errMsg);
                    break;
                }
                retcode = SQLNumParams(hstmt, &m_parameters);
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_STMT, hstmt, errMsg);
                    break;
                } else if (/*m_vPInfo.size() && */m_parameters != (SQLSMALLINT) m_vPInfo.size()) {
                    res = SPA::Odbc::ER_BAD_PARAMETER_COLUMN_SIZE;
                    errMsg = BAD_PARAMETER_COLUMN_SIZE;
                    break;
                }
                m_outputs = 0;
                for (auto it = m_vPInfo.begin(), end = m_vPInfo.end(); it != end; ++it) {
                    switch (it->DataType) {
                        case VT_I1:
                        case VT_UI1:
                        case VT_I2:
                        case VT_UI2:
                        case VT_I4:
                        case VT_UI4:
                        case VT_INT:
                        case VT_UINT:
                        case VT_I8:
                        case VT_UI8:
                        case VT_R4:
                        case VT_R8:
                        case VT_DATE:
                        case VT_DECIMAL:
                        case VT_CLSID:
                        case VT_BSTR:
                        case (VT_UI1 | VT_ARRAY):
                        case (VT_I1 | VT_ARRAY):
                        case VT_BOOL:
                            break;
                        default:
                            res = SPA::Odbc::ER_BAD_INPUT_PARAMETER_DATA_TYPE;
                            errMsg = BAD_INPUT_PARAMETER_DATA_TYPE;
                            break;
                    }
                    switch (it->Direction) {
                        case pdUnknown:
                            res = SPA::Odbc::ER_BAD_PARAMETER_DIRECTION_TYPE;
                            errMsg = BAD_PARAMETER_DIRECTION_TYPE;
                            break;
                        case pdInput:
                            break;
                        default:
                            ++m_outputs;
                            break;
                    }
                    if (res)
                        break;
                }
                parameters = m_outputs;
                parameters <<= 16;
                parameters += (unsigned short) (m_parameters - m_outputs);
            } while (false);
            if (res) {
                m_pExcuting.reset();
                m_pPrepare.reset();
                m_vPInfo.clear();
                m_parameters = 0;
                m_outputs = 0;
                parameters = 0;
            }
        }

        bool COdbcImpl::CheckInputParameterDataTypes() {
            m_nParamDataSize = 0;
            unsigned int rows = (unsigned int) (m_vParam.size() / (unsigned short) m_parameters);
            for (SQLSMALLINT n = 0; n < m_parameters; ++n) {
                CParameterInfo &info = m_vPInfo[n];
                switch (info.DataType) {
                    case VT_I1:
                    case VT_UI1:
                        m_nParamDataSize += sizeof (unsigned char);
                        break;
                    case VT_BOOL:
                        m_nParamDataSize += sizeof (bool);
                        break;
                    case VT_I2:
                    case VT_UI2:
                        m_nParamDataSize += sizeof (unsigned short);
                        break;
                    case VT_INT:
                    case VT_UINT:
                    case VT_R4:
                    case VT_I4:
                    case VT_UI4:
                        m_nParamDataSize += sizeof (int);
                        break;
                    case VT_R8:
                    case VT_I8:
                    case VT_UI8:
                    case VT_DATE:
                        m_nParamDataSize += sizeof (double);
                        break;
                    case VT_DECIMAL:
                        //convert decimal into ASCII string
                        m_nParamDataSize += sizeof (SQLPOINTER);
                        break;
                    case VT_BSTR:
                    case (VT_I1 | VT_ARRAY):
                    case (VT_UI1 | VT_ARRAY):
                        m_nParamDataSize += sizeof (SQLPOINTER);
                        break;
                    default:
                        break;
                }
                if (info.Direction != pdInput && info.Direction != pdInputOutput) {
                    continue;
                }
                for (unsigned int r = 0; r < rows; ++r) {
                    CDBVariant &d = m_vParam[(unsigned int) n + r * ((unsigned int) m_parameters)];
                    VARTYPE vtP = d.vt;
                    if (vtP == VT_NULL || vtP == VT_EMPTY) {
                        continue;
                    }

                    //ignore signed/unsigned matching
                    if (vtP == VT_UI1) {
                        vtP = VT_I1;
                    } else if (vtP == VT_UI2) {
                        vtP = VT_I2;
                    } else if (vtP == VT_UI4 || vtP == VT_INT || vtP == VT_UINT) {
                        vtP = VT_I4;
                    } else if (vtP == VT_UI8) {
                        vtP = VT_I8;
                    }
                    VARTYPE vt = info.DataType;
                    if (vt == VT_UI1) {
                        vt = VT_I1;
                    } else if (vt == VT_UI2) {
                        vt = VT_I2;
                    } else if (vt == VT_UI4 || vt == VT_INT || vt == VT_UINT) {
                        vtP = VT_I4;
                    } else if (vt == VT_UI8) {
                        vt = VT_I8;
                    }

                    if (vt != vtP) {
                        return false;
                    }

                    if (vtP == VT_DECIMAL) {
                        ConvertDecimalAString(d);
                    }
                }
            }
            return true;
        }

        bool COdbcImpl::BindParameters(CUQueue & q) {
            unsigned int rows = (unsigned int) ((m_vParam.size() / (unsigned short) m_parameters));
            unsigned int size = rows * (sizeof (SQLUSMALLINT) + sizeof (SQLLEN) + m_nParamDataSize);
            if (q.GetMaxSize() < size) {
                q.ReallocBuffer(size);
            }
            q.SetSize(size);
            unsigned char *buffer = (unsigned char*) q.GetBuffer();
            memset(buffer, 0, size);
            unsigned int pos_status = 0;
            unsigned int pos_len_ind = rows * sizeof (SQLUSMALLINT);
            for (SQLSMALLINT n = 0; n < m_parameters; ++n) {
                CParameterInfo &info = m_vPInfo[n];
                for (unsigned int r = 0; r < rows; ++r) {
                    CDBVariant &d = m_vParam[(unsigned int) n + r * ((unsigned int) m_parameters)];

                }
            }
            return false;
        }

        void COdbcImpl::ConvertDecimalAString(CDBVariant & vt) {
            if (vt.vt = VT_DECIMAL) {
                const DECIMAL &decVal = vt.decVal;
                std::string s = std::to_string(decVal.Lo64);
                unsigned char len = (unsigned char) s.size();
                if (len <= decVal.scale) {
                    s.insert(0, (decVal.scale - len) + 1, '0');
                }
                if (decVal.sign) {
                    s.insert(0, 1, '-');
                }
                if (decVal.scale) {
                    size_t pos = s.length() - decVal.scale;
                    s.insert(pos, 1, '.');
                }
                vt = s.c_str();
            }
        }

        void COdbcImpl::ExecuteParameters(bool rowset, bool meta, bool lastInsertId, UINT64 index, INT64 &affected, int &res, std::wstring &errMsg, CDBVariant &vtId, UINT64 & fail_ok) {
            fail_ok = 0;
            affected = 0;
            UINT64 fails = m_fails;
            UINT64 oks = m_oks;
            do {
                if (!m_pOdbc) {
                    res = SPA::Odbc::ER_NO_DB_OPENED_YET;
                    errMsg = NO_DB_OPENED_YET;
                    ++m_fails;
                    break;
                }
                if (!m_pPrepare) {
                    res = SPA::Odbc::ER_NO_PARAMETER_SPECIFIED;
                    errMsg = NO_PARAMETER_SPECIFIED;
                    ++m_fails;
                    break;
                }
                SQLINTEGER ParamsProcessed = 0;
                SQLRETURN retcode = SQLSetStmtAttr(m_pPrepare.get(), SQL_ATTR_PARAMS_PROCESSED_PTR, &ParamsProcessed, 0);
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_STMT, m_pPrepare.get(), errMsg);
                    ++m_fails;
                    break;
                }
                CScopeUQueue sb;
                if (m_parameters) {
                    if ((m_vParam.size() % (unsigned short) m_parameters) || m_vParam.size() == 0) {
                        res = SPA::Odbc::ER_BAD_PARAMETER_DATA_ARRAY_SIZE;
                        errMsg = BAD_PARAMETER_DATA_ARRAY_SIZE;
                        ++m_fails;
                        break;
                    } else if (!CheckInputParameterDataTypes()) {
                        res = SPA::Odbc::ER_BAD_INPUT_PARAMETER_DATA_TYPE;
                        errMsg = BAD_INPUT_PARAMETER_DATA_TYPE;
                        ++m_fails;
                        break;
                    }
                    unsigned int rows = (unsigned int) ((m_vParam.size() / (unsigned short) m_parameters));
                    retcode = SQLSetStmtAttr(m_pPrepare.get(), SQL_ATTR_PARAMSET_SIZE, (SQLPOINTER) rows, 0);
                    if (!SQL_SUCCEEDED(retcode)) {
                        res = SPA::Odbc::ER_ERROR;
                        GetErrMsg(SQL_HANDLE_STMT, m_pPrepare.get(), errMsg);
                        ++m_fails;
                        break;
                    } else if (!BindParameters(*sb)) {
                        res = SPA::Odbc::ER_ERROR;
                        GetErrMsg(SQL_HANDLE_STMT, m_pPrepare.get(), errMsg);
                        ++m_fails;
                        break;
                    }
                }
                res = 0;
                retcode = SQLExecute(m_pPrepare.get());
                if (!SQL_SUCCEEDED(retcode)) {
                    res = SPA::Odbc::ER_ERROR;
                    GetErrMsg(SQL_HANDLE_STMT, m_pPrepare.get(), errMsg);
                    ++m_fails;
                    break;
                }
                do {
                    SQLSMALLINT columns = 0;
                    retcode = SQLNumResultCols(m_pPrepare.get(), &columns);
                    assert(SQL_SUCCEEDED(retcode));
                    if (columns) {
                        if (rowset) {
                            unsigned int ret;
                            CDBColumnInfoArray vInfo = GetColInfo(m_pPrepare.get(), columns, meta);
                            {
                                CScopeUQueue sbRowset;
                                sbRowset << vInfo << index;
                                ret = SendResult(idRowsetHeader, sbRowset->GetBuffer(), sbRowset->GetSize());
                            }
                            if (ret == REQUEST_CANCELED || ret == SOCKET_NOT_FOUND) {
                                return;
                            }
                            bool ok = PushRecords(m_pPrepare.get(), vInfo, res, errMsg);
                            if (!ok) {
                                return;
                            }
                            if (res) {
                                ++m_fails;
                            } else {
                                ++m_oks;
                            }
                        }
                    } else {
                        SQLLEN rows = 0;
                        retcode = SQLRowCount(m_pPrepare.get(), &rows);
                        assert(SQL_SUCCEEDED(retcode));
                        if (rows > 0) {
                            affected += rows;
                        }
                        ++m_oks;
                    }
                } while (SQLMoreResults(m_pPrepare.get()) == SQL_SUCCESS);
            } while (false);
            fail_ok = ((m_fails - fails) << 32);
            fail_ok += (unsigned int) (m_oks - oks);
        }

        void COdbcImpl::StartBLOB(unsigned int lenExpected) {
            m_Blob.SetSize(0);
            if (lenExpected > m_Blob.GetMaxSize()) {
                m_Blob.ReallocBuffer(lenExpected);
            }
            CUQueue &q = m_UQueue;
            m_Blob.Push(q.GetBuffer(), q.GetSize());
            assert(q.GetSize() > sizeof (unsigned short) + sizeof (unsigned int));
            q.SetSize(0);
        }

        void COdbcImpl::Chunk() {
            CUQueue &q = m_UQueue;
            if (q.GetSize()) {
                m_Blob.Push(q.GetBuffer(), q.GetSize());
                q.SetSize(0);
            }
        }

        void COdbcImpl::EndBLOB() {
            Chunk();
            m_vParam.push_back(CDBVariant());
            CDBVariant &vt = m_vParam.back();
            m_Blob >> vt;
            assert(m_Blob.GetSize() == 0);
        }

        void COdbcImpl::BeginRows() {
            Transferring();
        }

        void COdbcImpl::EndRows() {
            Transferring();
        }

        void COdbcImpl::Transferring() {
            CUQueue &q = m_UQueue;
            while (q.GetSize()) {
                m_vParam.push_back(CDBVariant());
                CDBVariant &vt = m_vParam.back();
                q >> vt;
            }
            assert(q.GetSize() == 0);
        }

        void COdbcImpl::GetErrMsg(SQLSMALLINT HandleType, SQLHANDLE Handle, std::wstring & errMsg) {
            static std::wstring SQLSTATE(L"SQLSTATE=");
            static std::wstring NATIVE_ERROR(L":NATIVE=");
            static std::wstring ERROR_MESSAGE(L":ERROR_MESSAGE=");
            errMsg.clear();
            SQLSMALLINT i = 1, MsgLen = 0;
            SQLINTEGER NativeError = 0;
            SQLCHAR SqlState[6] = {0}, Msg[SQL_MAX_MESSAGE_LENGTH + 1] = {0};
            SQLRETURN res = SQLGetDiagRec(HandleType, Handle, i, SqlState, &NativeError, Msg, sizeof (Msg) / sizeof (SQLCHAR), &MsgLen);
            while (res != SQL_NO_DATA) {
                if (errMsg.size())
                    errMsg += L";";
                errMsg += (SQLSTATE + SPA::Utilities::ToWide((const char*)SqlState));
                errMsg += (NATIVE_ERROR + std::to_wstring((INT64) NativeError));
                errMsg += (ERROR_MESSAGE + SPA::Utilities::ToWide((const char*)Msg));
                ::memset(Msg, 0, sizeof (Msg));
                ++i;
                MsgLen = 0;
                NativeError = 0;
                res = SQLGetDiagRec(HandleType, Handle, i, SqlState, &NativeError, Msg, sizeof (Msg), &MsgLen);
            }
        }
    } //namespace ServerSide
} //namespace SPA