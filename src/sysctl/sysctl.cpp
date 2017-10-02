/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOST_FILESYSTEM_VERSION 3
#define BOOST_FILESYSTEM_NO_DEPRECATED
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <system_error>
#include <iostream>

extern "C"
{
/* #include "config.h" */
#include "conf-files.h"
#include "def.h"
#include "fd-util.h"
#include "fileio.h"
/* #include "hashmap.h" */
#include "log.h"
#include "path-util.h"
#include "string-util.h"
#include "strv.h"
#include "sysctl-util.h"
#include "util.h"
}

namespace bpt = boost::property_tree;
namespace bpo = boost::program_options;
namespace bf = boost::filesystem;

static const char conf_file_dirs[] = CONF_PATHS_NULSTR("sysctl.d");

/* This is a C++ version of a function that really lives in sysctl-util.{c,h} */
std::string sysctl_normalize_cpp(std::string s) {
        bool found_dot_already = false;
        for(char& c : s) {
                /* If the first separator is a slash, the path is
                 * assumed to be normalized and slashes remain slashes
                 * and dots remains dots.
                 * Otherwise, dots become slashes and slashes become
                 * dots. Fun. */
                if ('/' == c) {
                        if (!found_dot_already)
                                return s;
                        c = '.';
                }
                else if ('.' == c) {
                        found_dot_already = true;
                        c = '/';
                }
        }
        return s;
}

static bool test_prefix(const std::string &p, const std::vector<std::string> &prefixes) {
        if (prefixes.empty())
                return true;

        for (auto& i : prefixes) {
                const char *t;

                t = path_startswith(i.c_str(), "/proc/sys/");
                if (!t)
                        t = i.c_str();
                if (path_startswith(p.c_str(), t))
                        return true;
        }
        return false;
}

static std::error_code apply_all(const bpt::ptree &sysctl_options, const std::vector<std::string> &arg_prefixes) {
        std::error_code r;

        for (auto& entry : sysctl_options) {
                std::error_code k;

                // Skip paths that don't match the prefix whitelist.
                if (!test_prefix(entry.first.data(), arg_prefixes)) {
                        continue;
                }

                /* Values are already trimmed for whitespace by the INI parser.
                 * We only have to normalize them. */
                auto value = sysctl_normalize_cpp(entry.second.data());

                k.assign(sysctl_write(entry.first.data(), value.c_str()), std::generic_category());
                if (k) {
                        /* If the sysctl is not available in the kernel or we are running with reduced privileges and
                         * cannot write it, then log about the issue at LOG_NOTICE level, and proceed without
                         * failing. (EROFS is treated as a permission problem here, since that's how container managers
                         * usually protected their sysctls.) In all other cases log an error and make the tool fail. */

                        if (IN_SET(k.value(), -EPERM, -EACCES, -EROFS, -ENOENT))
                                log_notice_errno(k.value(), "Couldn't write '%s' to '%s', ignoring: %m", value.c_str(), entry.first.data());
                        else {
                                log_error_errno(k.value(), "Couldn't write '%s' to '%s': %m", value.c_str(), entry.first.data());
                                if (!r)
                                        r = k;
                        }
                }
        }

        return r;
}

static std::error_code parse_file(bpt::ptree &sysctl_options, const boost::filesystem::path &path, bool ignore_enoent) {
        std::error_code r;

        log_debug("Parsing %s", path.c_str());
        for (;;) {
                try {
                        bpt::read_ini(path.generic_string(), sysctl_options);
                } catch (bpt::ini_parser_error pe) {
                        // @TODO: Handle ENOENT (and possibly ignoring it) here.
                        // log_error_errno(r, "Failed to open file '%s', ignoring: %m", path);

                        log_debug("Error: %s at '%s:%lu'.", pe.message().c_str(), pe.filename().c_str(), pe.line());
                        r.assign(EINVAL, std::generic_category());
                        return r;
                }

                /* Filtering is now handled on applying the values. */
        }

        return r;
}

static std::error_code parse_argv(int argc, char *argv[], std::vector<std::string> &arg_conf_files, std::vector<std::string> &arg_prefixes) {
        std::error_code r;

        bpo::options_description desc("Applies kernel sysctl settings.");
        desc.add_options()
                ("help,h", "Show this help")
                ("version", "Show package version")
                ("prefix", bpo::value<std::vector<std::string>>(&arg_prefixes), "Only apply rules with the specified path prefix(es)")
                ("configuration-file", bpo::value<std::vector<std::string>>(&arg_conf_files), "Path(s) to listing(s) of sysctl settings to apply");

        bpo::positional_options_description p;
        p.add("configuration-file", -1);

        auto parsed = bpo::command_line_parser(argc, argv).options(desc).positional(p).run();
        bpo::variables_map vm;
        bpo::store(parsed, vm);
        bpo::notify(vm);

        if (vm.count("help")) {
                //printf("%s %s", program_invocation_short_name, desc);
                std::cout << program_invocation_short_name << " " << desc << "\n";
                r.assign(EINVAL, std::generic_category());
                return r;
        }

        if (vm.count("version")) {
                // @TODO: Actually use the version() return value.
                version();
                r.assign(EINVAL, std::generic_category());
                return r;
        }

        for (auto& prefix : arg_prefixes) {
                prefix = sysctl_normalize_cpp(prefix);
                if (!path_startswith(prefix.c_str(), "/proc/sys"))
                        prefix = "/proc/sys/" + prefix;
        }

        return r;
}

int main(int argc, char *argv[]) {
        bpt::ptree sysctl_options;
        std::vector<std::string> arg_conf_files;
	std::vector<std::string> arg_prefixes;
        std::error_code r, k;

        r = parse_argv(argc, argv, arg_conf_files, arg_prefixes);
        if (r)
                return EXIT_FAILURE;

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        umask(0022);

        r.clear();

        if (!arg_conf_files.empty()) {
                for (auto &f : arg_conf_files) {
                        k = parse_file(sysctl_options, f, false);
                        if (k && !r)
                                r = k;
                }
        } else {
                bf::path p(conf_file_dirs);

                // @TODO: Add better error handling or trap exceptions if it's
                // not a directory or unreadable.

                for (auto& f : bf::directory_iterator(p)) {
                        k = parse_file(sysctl_options, f.path(), true);
                        if (k && !r)
                                r = k;
                }
        }

        k = apply_all(sysctl_options, arg_prefixes);
        if (k && !r)
                r = k;

        return r ? EXIT_FAILURE : EXIT_SUCCESS;
}
