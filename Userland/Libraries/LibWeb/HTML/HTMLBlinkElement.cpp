/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/HTMLBlinkElement.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Platform/Timer.h>

namespace Web::HTML {

HTMLBlinkElement::HTMLBlinkElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
    , m_timer(Platform::Timer::create())
{
    m_timer->set_interval(500);
    m_timer->on_timeout = [this] { blink(); };
    m_timer->start();
}

HTMLBlinkElement::~HTMLBlinkElement() = default;

void HTMLBlinkElement::blink()
{
    if (!layout_node())
        return;

    layout_node()->set_visible(!layout_node()->is_visible());
    layout_node()->set_needs_display();
}

}
