/*
 * Copyright: 2013 Canonical, Ltd
 *
 * This file is part of reminders
 *
 * reminders is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * reminders is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

import QtQuick 2.4
import Ubuntu.Components 1.3
import Ubuntu.Components.Popups 1.3

Item {
    id: spacerItem
    objectName: "__ToolBarItems.SpacerItem"
    height: parent.height
    width: {
        var othersWidth = 0;

        // Hack: the autogenerated back button
        if (pageStack != null && pageStack.depth > 1) {
            othersWidth = units.gu(6)
        }

        for (var i = 0; i < parent.children.length; ++i) {
            var otherItem = parent.children[i];
            if (otherItem.objectName !== "__ToolBarItems.SpacerItem") {
                if (otherItem.visible) {
                    othersWidth += otherItem.width + parent.spacing;
                }
            }
        }
        return parent.parent.width - othersWidth - units.gu(4);
    }
}
