/***********************************************************************************
* Copyright (c) 2016, Johan Mabille, Loic Gouarin, Sylvain Corlay, Wolf Vollprecht *
* Copyright (c) 2016, QuantStack                                                   *
*                                                                                  *
* Distributed under the terms of the BSD 3-Clause License.                         *
*                                                                                  *
* The full license is in the file LICENSE, distributed with this software.         *
************************************************************************************/

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <regex>
#include <sstream>
#include <vector>

#include "llvm/Support/DynamicLibrary.h"
#include "xeus-cling/xbuffer.hpp"
#include "xeus-cling/xeus_cling_config.hpp"
#include "xeus-cling/xinterpreter.hpp"
#include "xeus-cling/xmagics.hpp"

#include "xinput.hpp"
#include "xinspect.hpp"
#include "xmagics/executable.hpp"
#include "xmagics/execution.hpp"
#include "xmagics/os.hpp"
#include "xmime_internal.hpp"
#include "xparser.hpp"
#include "xsystem.hpp"

using namespace std::placeholders;

namespace xcpp
{
    void interpreter::configure_impl()
    {
        // Process #include "xeus/xinterpreter.hpp" in a separate block.
        m_interpreter.process("#include \"xeus/xinterpreter.hpp\"", nullptr, nullptr, true);
        // Expose interpreter instance to cling
        std::string block = "xeus::register_interpreter(static_cast<xeus::xinterpreter*>((void*)" + std::to_string(intptr_t(this)) + "));";
        m_interpreter.process(block.c_str(), nullptr, nullptr, true);
    }

    interpreter::interpreter(int argc, const char* const* argv)
        : m_interpreter(argc, argv, LLVM_DIR),
          m_input_validator(),
          m_version(get_stdopt(argc, argv)), // Extract C++ language standard version from command-line option
          xmagics(),
          p_cout_strbuf(nullptr), p_cerr_strbuf(nullptr),
          m_cout_buffer(std::bind(&interpreter::publish_stdout, this, _1)),
          m_cerr_buffer(std::bind(&interpreter::publish_stderr, this, _1))
    {
        // Extra setup normally performed in Cling's main function
        m_cling.AddIncludePath(".");

        // Load libs
        const cling::InvocationOptions& Opts = m_cling.getOptions();
        for (const std::string& Lib : Opts.LibsToLoad)
        {
          m_cling.loadFile(Lib);
        }

        redirect_output();
        init_preamble();
        init_magic();
    }

    interpreter::~interpreter()
    {
        restore_output();
    }

    nl::json interpreter::execute_request_impl(int execution_counter,
                                               const std::string& code,
                                               bool silent,
                                               bool /*store_history*/,
                                               nl::json /*user_expressions*/,
                                               bool allow_stdin)
    {
        nl::json kernel_res;

        // Check for magics
        for (auto& pre : preamble_manager.preamble)
        {
            if (pre.second.is_match(code))
            {
                pre.second.apply(code, kernel_res);
                return kernel_res;
            }
        }

        // Split code from includes
        auto blocks = split_from_includes(code.c_str());

        auto errorlevel = 0;

        std::string ename;
        std::string evalue;
        cling::Value output;
        cling::Interpreter::CompilationResult compilation_result;

        // If silent is set to true, temporarily dismiss all std::cerr and
        // std::cout outputs resulting from `m_interpreter.process`.

        auto cout_strbuf = std::cout.rdbuf();
        auto cerr_strbuf = std::cerr.rdbuf();

        if (silent)
        {
            auto null = xnull();
            std::cout.rdbuf(&null);
            std::cerr.rdbuf(&null);
        }

        // Scope guard performing the temporary redirection of input requests.
        auto input_guard = input_redirection(allow_stdin);

        for (const auto& block : blocks)
        {
            // Attempt normal evaluation
            try
            {
                compilation_result = m_interpreter.process(block, &output, nullptr, true);
            }

            // Catch all errors
            catch (cling::InterpreterException& e)
            {
                errorlevel = 1;
                ename = "Interpreter Exception";
                if (!e.diagnose())
                {
                    evalue = e.what();
                }
            }
            catch (std::exception& e)
            {
                errorlevel = 1;
                ename = "Standard Exception";
                evalue = e.what();
            }
            catch (...)
            {
                errorlevel = 1;
                ename = "Error";
            }

            if (compilation_result != cling::Interpreter::kSuccess)
            {
                errorlevel = 1;
                ename = "Interpreter Error";
            }

            // If an error was encountered, don't attempt further execution
            if (errorlevel)
            {
                break;
            }
        }

        // Flush streams
        std::cout << std::flush;
        std::cerr << std::flush;

        // Reset non-silent output buffers
        if (silent)
        {
            std::cout.rdbuf(cout_strbuf);
            std::cerr.rdbuf(cerr_strbuf);
        }

        // Depending of error level, publish execution result or execution
        // error, and compose execute_reply message.
        if (errorlevel)
        {
            // Classic Notebook does not make use of the "evalue" or "ename"
            // fields, and only displays the traceback.
            //
            // JupyterLab displays the "{ename}: {evalue}" if the traceback is
            // empty.
            std::vector<std::string> traceback({ename + ": " + evalue});
            if (!silent)
            {
                publish_execution_error(ename, evalue, traceback);
            }

            // Compose execute_reply message.
            kernel_res["status"] = "error";
            kernel_res["ename"] = ename;
            kernel_res["evalue"] = evalue;
            kernel_res["traceback"] = traceback;
        }
        else
        {
            // Publish a mime bundle for the last return value if
            // the semicolon was omitted.
            if (!silent && output.hasValue() && trim(blocks.back()).back() != ';')
            {
                nl::json pub_data = mime_repr(output);
                publish_execution_result(execution_counter, std::move(pub_data), nl::json::object());
            }

            // Compose execute_reply message.
            kernel_res["status"] = "ok";
            kernel_res["payload"] = nl::json::array();
            kernel_res["user_expressions"] = nl::json::object();
        }
        return kernel_res;
    }

    nl::json interpreter::complete_request_impl(const std::string& code,
                                                int cursor_pos)
    {
        std::vector<std::string> result;
        cling::Interpreter::CompilationResult compilation_result;
        nl::json kernel_res;

        // split the input to have only the word in the back of the cursor
        std::string delims = " \t\n`!@#$^&*()=+[{]}\\|;:\'\",<>?.";
        std::size_t _cursor_pos = cursor_pos;
        auto text = split_line(code, delims, _cursor_pos);
        std::string to_complete = text.back().c_str();

        compilation_result = m_interpreter.codeComplete(code.c_str(), _cursor_pos, result);

        // change the print result
        for (auto& r : result)
        {
            // remove the definition at the beginning (for example [#int#])
            r = std::regex_replace(r, std::regex("\\[\\#.*\\#\\]"), "");
            // remove the variable name in <#type name#>
            r = std::regex_replace(r, std::regex("(\\ |\\*)+(\\w+)(\\#\\>)"), "$1$3");
            // remove unnecessary space at the end of <#type   #>
            r = std::regex_replace(r, std::regex("\\ *(\\#\\>)"), "$1");
            // remove <# #> to keep only the type
            r = std::regex_replace(r, std::regex("\\<\\#([^#>]*)\\#\\>"), "$1");
        }

        kernel_res["matches"] = result;
        kernel_res["cursor_start"] = cursor_pos - to_complete.length();
        kernel_res["cursor_end"] = cursor_pos;
        kernel_res["metadata"] = nl::json::object();
        kernel_res["status"] = "ok";
        return kernel_res;
    }

    nl::json interpreter::inspect_request_impl(const std::string& code,
                                               int cursor_pos,
                                               int /*detail_level*/)
    {
        nl::json kernel_res;

        auto dummy = code.substr(0, cursor_pos);
        // TODO: same pattern as in inspect function (keep only one)
        std::string exp = R"(\w*(?:\:{2}|\<.*\>|\(.*\)|\[.*\])?)";
        std::regex re_method{"(" + exp + R"(\.?)*$)"};
        std::smatch magic;
        if (std::regex_search(dummy, magic, re_method))
        {
            inspect(magic[0], kernel_res, m_interpreter);
        }
        return kernel_res;
    }

    nl::json interpreter::is_complete_request_impl(const std::string& code)
    {
        nl::json kernel_res;

        m_input_validator.reset();
        cling::InputValidator::ValidationResult Res = m_input_validator.validate(code);
        if (Res == cling::InputValidator::kComplete)
        {
            kernel_res["status"] = "complete";
        }
        else if (Res == cling::InputValidator::kIncomplete)
        {
            kernel_res["status"] = "incomplete";
        }
        else if (Res == cling::InputValidator::kMismatch)
        {
            kernel_res["status"] = "invalid";
        }
        else
        {
            kernel_res["status"] = "unknown";
        }
        kernel_res["indent"] = "";
        return kernel_res;
    }

    nl::json interpreter::kernel_info_request_impl()
    {
        nl::json result;
        result["implementation"] = "xeus-cling";
        result["implementation_version"] = XEUS_CLING_VERSION;

        /* The jupyter-console banner for xeus-cling is the following:
          __  _____ _   _ ___ 
          \ \/ / _ \ | | / __|
           >  <  __/ |_| \__ \
          /_/\_\___|\__,_|___/

          xeus-cling: a Jupyter Kernel C++ - based on cling
        */

        std::string banner = ""
              "  __  _____ _   _ ___\n"
              "  \\ \\/ / _ \\ | | / __|\n"
              "   >  <  __/ |_| \\__ \\\n"
              "  /_/\\_\\___|\\__,_|___/\n"
              "\n"
              "  xeus-cling: a Jupyter Kernel C++ - based on cling\n"
              "  C++";
        banner.append(m_version);
        result["banner"] = banner;
        result["language_info"]["name"] = "c++";
        result["language_info"]["version"] = m_version;
        result["language_info"]["mimetype"] = "text/x-c++src";
        result["language_info"]["codemirror_mode"] = "text/x-c++src";
        result["language_info"]["file_extension"] = ".cpp";
        result["help_links"] = nl::json::array();
        result["help_links"][0] = nl::json::object({
            {"text", "Xeus-Cling Reference"},
            {"url", "https://xeus-cling.readthedocs.io"}
        });
        result["status"] = "ok";
        return result;
    }

    void interpreter::shutdown_request_impl()
    {
        restore_output();
    }

    static std::string c_format(const char* format, std::va_list args)
    {
        // Call vsnprintf once to determine the required buffer length. The
        // return value is the number of characters _excluding_ the null byte.
        std::va_list args_bufsz;
        va_copy(args_bufsz, args);
        std::size_t bufsz = vsnprintf(NULL, 0, format, args_bufsz);
        va_end(args_bufsz);

        // Create an empty string of that size.
        std::string s(bufsz, 0);

        // Now format the data into this string and return it.
        std::va_list args_format;
        va_copy(args_format, args);
        // The second parameter is the maximum number of bytes that vsnprintf
        // will write _including_ the  terminating null byte.
        vsnprintf(&s[0], s.size() + 1, format, args_format);
        va_end(args_format);

        return s;
    }

    static int printf_jit(const char* format, ...)
    {
        std::va_list args;
        va_start(args, format);

        std::string buf = c_format(format, args);
        std::cout << buf;

        va_end(args);

        return buf.size();
    }

    static int fprintf_jit(std::FILE* stream, const char* format, ...)
    {
        std::va_list args;
        va_start(args, format);

        int ret;
        if (stream == stdout || stream == stderr)
        {
            std::string buf = c_format(format, args);
            if (stream == stdout)
            {
                std::cout << buf;
            }
            else if (stream == stderr)
            {
                std::cerr << buf;
            }
            ret = buf.size();
        }
        else
        {
            // Just forward to vfprintf.
            ret = vfprintf(stream, format, args);
        }

        va_end(args);

        return ret;
    }

    void interpreter::redirect_output()
    {
        p_cout_strbuf = std::cout.rdbuf();
        p_cerr_strbuf = std::cerr.rdbuf();

        std::cout.rdbuf(&m_cout_buffer);
        std::cerr.rdbuf(&m_cerr_buffer);

        // Inject versions of printf and fprintf that output to std::cout
        // and std::cerr (see implementation above).
        llvm::sys::DynamicLibrary::AddSymbol("printf", (void*) &printf_jit);
        llvm::sys::DynamicLibrary::AddSymbol("fprintf", (void*) &fprintf_jit);
    }

    void interpreter::restore_output()
    {
        std::cout.rdbuf(p_cout_strbuf);
        std::cerr.rdbuf(p_cerr_strbuf);

        // No need to remove the injected versions of [f]printf: As they forward
        // to std::cout and std::cerr, these are handled implicitly.
    }

    void interpreter::publish_stdout(const std::string& s)
    {
        publish_stream("stdout", s);
    }

    void interpreter::publish_stderr(const std::string& s)
    {
        publish_stream("stderr", s);
    }

    void interpreter::init_preamble()
    {
        preamble_manager.register_preamble("introspection", new xintrospection(m_interpreter));
        preamble_manager.register_preamble("magics", new xmagics_manager());
        preamble_manager.register_preamble("shell", new xsystem());
    }

    void interpreter::init_magic()
    {
        preamble_manager["magics"].get_cast<xmagics_manager>().register_magic("executable", executable(m_interpreter));
        preamble_manager["magics"].get_cast<xmagics_manager>().register_magic("file", writefile());
        preamble_manager["magics"].get_cast<xmagics_manager>().register_magic("timeit", timeit(&m_interpreter));
    }

    std::string interpreter::get_stdopt(int argc, const char* const* argv)
    {
        std::string res = "11";
        for (int i = 0; i < argc; ++i)
        {
            std::string tmp(argv[i]);
            auto pos = tmp.find("-std=c++");
            if (pos != std::string::npos)
            {
                res = tmp.substr(pos + 8);
                break;
            }
        }
        return res;
    }
}
