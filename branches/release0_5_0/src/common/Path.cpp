/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2007 Christian Schoenebeck                              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This library is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this library; if not, write to the Free Software           *
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston,                 *
 *   MA  02111-1307  USA                                                   *
 ***************************************************************************/

#include "Path.h"

// for function hexsToNumber()
#include "global_private.h"

namespace LinuxSampler {

void Path::appendNode(std::string Name) {
    if (!Name.size()) return;
    elements.push_back(Name);
}

std::string Path::toPosix() {
    // POSIX paths consist of forward slash separators and requires forward
    // slashes in path and file names to be encoded as "%2f", the file names
    // "." and ".." have special meanings
    // (see http://www.opengroup.org/onlinepubs/000095399/basedefs/xbd_chap03.html#tag_03_169
    // and http://www.opengroup.org/onlinepubs/000095399/basedefs/xbd_chap03.html#tag_03_266 )
    std::string result;
    for (int iElement = 0; iElement < elements.size(); iElement++) {
        // encode percent characters
        std::string e = elements[iElement];
        for (
            int pos = e.find("%"); pos != std::string::npos;
            pos = e.find("%", pos+2)
        ) e.replace(pos/*offset*/, 1/*length*/, "%%"/*by*/);
        // encode forward slashes
        for (
            int pos = e.find("/"); pos != std::string::npos;
            pos = e.find("/", pos+3)
        ) e.replace(pos/*offset*/, 1/*length*/, "%2f"/*by*/);
        // append encoded node to full encoded path
        result += "/" + e;
    }
    if (!result.size()) result = "/";
    return result;
}

std::string Path::toDbPath() {
    std::string result;
    for (int iElement = 0; iElement < elements.size(); iElement++) {
        // replace all slashes with '\0'
        std::string e = elements[iElement];
        for (int i = 0; i < e.length(); i++) {
            if (e.at(i) == '/') e.at(i) = '\0';
        }
        // append encoded node to full encoded path
        result += "/" + e;
    }
    if (!result.size()) result = "/";
    return result;
}

std::string Path::toLscp() {
    std::string result;
    for (int iElement = 0; iElement < elements.size(); iElement++) {
        // replace "special characters" by LSCP escape sequences
        std::string e = elements[iElement];
        for (int i = 0; i < e.length(); i++) {
            const char c = e.c_str()[i];
            if (
                !(c >= '0' && c <= '9') &&
                !(c >= 'a' && c <= 'z') &&
                !(c >= 'A' && c <= 'Z') &&
                !(c == '!') && !(c == '#') && !(c == '$') && !(c == '%') &&
                !(c == '&') && !(c == '(') && !(c == ')') && !(c == '*') &&
                !(c == '+') && !(c == ',') && !(c == '-') && !(c == '.') &&
                !(c == ':') && !(c == ';') && !(c == '<') && !(c == '=') &&
                !(c == '>') && !(c == '?') && !(c == '@') && !(c == '[') &&
                !(c == ']') && !(c == '^') && !(c == '_') && !(c == '`') &&
                !(c == '{') && !(c == '|') && !(c == '}') && !(c == '~')
            ) {
                // convert the "special" character into a "\xHH" LSCP escape sequence
                char buf[5];
                snprintf(buf, sizeof(buf), "\\x%02x", static_cast<unsigned char>(c));
                e.replace(i, 1, buf);
                i += 3;
            }
        }
        // append encoded node to full encoded path
        result += "/" + e;
    }
    if (!result.size()) result = "/";
    return result;
}

Path Path::operator+(const Path& p) {
    Path result = *this;
    for (int i = 0; i < p.elements.size(); i++)
        result.elements.push_back(p.elements[i]);
    return result;
}

Path Path::operator+(const Path* p) {
    return *this + *p;
}

Path Path::fromPosix(std::string path) {
    Path result;
    // first split the nodes
    {
        int nodeEnd;
        for (
            int nodeBegin = path.find_first_not_of('/');
            nodeBegin != std::string::npos;
            nodeBegin = path.find_first_not_of('/', nodeEnd)
        ) {
            nodeEnd = path.find_first_of('/', nodeBegin);
            result.appendNode(
                (nodeEnd != std::string::npos) ?
                    path.substr(nodeBegin, nodeEnd - nodeBegin) :
                    path.substr(nodeBegin)
            );
        }
    }
    // resolve POSIX escape sequences in all nodes
    for (int iNode = 0; iNode < result.elements.size(); iNode++) {
        std::string& s = result.elements[iNode];
        for (int pos = s.find('%'); pos < s.length(); pos = s.find('%', ++pos)) {
            if (pos+1 >= s.length()) { // unexpected character
                //TODO: we might want to throw an exception here, for now we simply replace the character by '?'
                s.replace(pos, 1, "?");
                continue;
            }
            if (s.c_str()[pos+1] == '%') {
                s.replace(pos, 2, "%");
                continue;
            }
            if (pos+2 >= s.length()) {
                //TODO: we might want to throw an exception here, for now we simply replace the character by '?'
                s.replace(pos, 2, "?");
                continue;
            }
            // expecting a "%HH" sequence here, convert it into the respective character
            const std::string sHex = s.substr(pos+1, 2);
            char cAscii = hexsToNumber(sHex.c_str()[1], sHex.c_str()[0]);
            char pcAscii[] = { cAscii, 0 };
            s.replace(pos, 3, pcAscii);
        }
    }
    return result;
}

Path Path::fromDbPath(std::string path) {
    Path result;
    // first split the nodes
    {
        int nodeEnd;
        for (
            int nodeBegin = path.find_first_not_of('/');
            nodeBegin != std::string::npos;
            nodeBegin = path.find_first_not_of('/', nodeEnd)
        ) {
            nodeEnd = path.find_first_of('/', nodeBegin);

            std::string s = (nodeEnd != std::string::npos) ?
                path.substr(nodeBegin, nodeEnd - nodeBegin) :
                path.substr(nodeBegin);

            for (int i = 0; i < s.length(); i++) if (s.at(i) == '\0') s.at(i) = '/';
            result.appendNode(s);
        }
    }
    return result;
}

} // namespace LinuxSampler
