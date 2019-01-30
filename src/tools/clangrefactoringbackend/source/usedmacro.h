/****************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#pragma once

#include <filepathid.h>

#include <utils/smallstring.h>

#include <vector>

namespace ClangBackEnd {

class UsedMacro
{
public:
    constexpr UsedMacro() = default;
    UsedMacro(Utils::SmallStringView macroName, FilePathId filePathId)
        : macroName(macroName),
          filePathId(filePathId)
    {}

    UsedMacro(Utils::SmallStringView macroName, int filePathId)
        : macroName(macroName),
          filePathId(filePathId)
    {}

    friend bool operator<(const UsedMacro &first, const UsedMacro &second)
    {
        return std::tie(first.filePathId, first.macroName)
             < std::tie(second.filePathId, second.macroName);
    }

    friend bool operator==(const UsedMacro &first, const UsedMacro &second)
    {
        return first.filePathId == second.filePathId && first.macroName == second.macroName;
    }

    friend bool operator!=(const UsedMacro &first, const UsedMacro &second)
    {
        return !(first == second);
    }

    operator Utils::SmallStringView() const { return macroName; }
    operator const Utils::SmallString &() const { return macroName; }

public:
    Utils::SmallString macroName;
    FilePathId filePathId;
};

using UsedMacros = std::vector<UsedMacro>;

} // ClangBackEnd
