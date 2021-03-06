/* ListDataEvent.java --
   Copyright (C) 2002 Free Software Foundation, Inc.

This file is part of GNU Classpath.

GNU Classpath is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

GNU Classpath is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Classpath; see the file COPYING.  If not, write to the
Free Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
02111-1307 USA.

Linking this library statically or dynamically with other modules is
making a combined work based on this library.  Thus, the terms and
conditions of the GNU General Public License cover the whole
combination.

As a special exception, the copyright holders of this library give you
permission to link this library with independent modules to produce an
executable, regardless of the license terms of these independent
modules, and to copy and distribute the resulting executable under
terms of your choice, provided that you also meet, for each linked
independent module, the terms and conditions of the license of that
module.  An independent module is a module which is not derived from
or based on this library.  If you modify this library, you may extend
this exception to your version of the library, but you are not
obligated to do so.  If you do not wish to do so, delete this
exception statement from your version. */


package javax.swing.event;

import java.util.EventObject;

/**
 * @author Andrew Selkirk
 * @author Ronald Veldema
 */
public class ListDataEvent extends EventObject
{
  private static final long serialVersionUID = 2510353260071004774L;
  
  public static final int CONTENTS_CHANGED = 0;
  public static final int INTERVAL_ADDED = 1;
  public static final int INTERVAL_REMOVED = 2;

  private int type = 0;
  private int index0 = 0;
  private int index1 = 0;
	
  /**
   * Creates a <code>ListDataEvent</code> object.
   * 
   * @param source The source of the event.
   * @param type The type of the event
   * @param index0 Bottom of range
   * @param index1 Top of range
   */
  public ListDataEvent(Object source, int type, int index0, int index1)
  {
    super(source);
    this.type = type;
    this.index0 = index0;
    this.index1 = index1;
  }
	
  /**
   * Returns the bottom index.
   */
  public int getIndex0()
  {
    return index0;
  }

  /**
   * Returns the top index.
   */
  public int getIndex1()
  {
    return index1;
  }

  /**
   * Returns the type of this event.
   */
  public int getType()
  {
    return type;
  }
}
