/**
 * Copyright 2016 Bartosz Schiller
 * <p/>
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * <p/>
 * http://www.apache.org/licenses/LICENSE-2.0
 * <p/>
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package com.github.barteksc.pdfviewer;

import android.graphics.PointF;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;
import android.util.Log;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import android.widget.Toast;

import com.github.barteksc.pdfviewer.exception.PageRenderingException;
import com.github.barteksc.pdfviewer.model.LinkTapEvent;
import com.github.barteksc.pdfviewer.scroll.ScrollHandle;
import com.github.barteksc.pdfviewer.util.SnapEdge;
import com.shockwave.pdfium.PdfDocument;
import com.shockwave.pdfium.util.Size;
import com.shockwave.pdfium.util.SizeF;

import java.util.ArrayList;
import java.util.HashMap;

import static com.github.barteksc.pdfviewer.util.Constants.Pinch.MAXIMUM_ZOOM;
import static com.github.barteksc.pdfviewer.util.Constants.Pinch.MINIMUM_ZOOM;

/**
 * This Manager takes care of moving the PDFView,
 * set its zoom track user actions.
 */
class DragPinchManager implements GestureDetector.OnGestureListener, GestureDetector.OnDoubleTapListener, ScaleGestureDetector.OnScaleGestureListener, View.OnTouchListener {
    float lastX;
    float lastY;
    float orgX;
    float orgY;
    private static final Object lock = new Object();

    Drawable draggingHandle;
    float lineHeight;

    float view_pager_toguard_lastX;
    float view_pager_toguard_lastY;
    PointF sCursorPosStart = new PointF();
    private PDFView pdfView;
    private AnimationManager animationManager;

    BreakIteratorHelper pageBreakIterator;
    String allText;
    private GestureDetector gestureDetector;
    private ScaleGestureDetector scaleGestureDetector;
    public long currentTextPtr;
    private boolean scrolling = false;
    private boolean scaling = false;
    private boolean enabled = false;

    DragPinchManager(PDFView pdfView, AnimationManager animationManager) {
        this.pdfView = pdfView;
        this.animationManager = animationManager;
        gestureDetector = new GestureDetector(pdfView.getContext(), this);
        scaleGestureDetector = new ScaleGestureDetector(pdfView.getContext(), this);
        pdfView.setOnTouchListener(this);
    }

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    void disableLongpress() {
        gestureDetector.setIsLongpressEnabled(false);
    }

    @Override
    public boolean onSingleTapConfirmed(MotionEvent e) {
        boolean onTapHandled = false;

        if (pdfView.hasSelection) {
            pdfView.clearSelection();
//            if (wordTapped(e.getX(), e.getY(), 1.5f)) {
//                if (pdfView.onSelection != null) {
//                    pdfView.onSelection.onSelection(true);
//                }
//                draggingHandle = pdfView.handleRight;
//                sCursorPosStart.set(pdfView.handleRightPos.right, pdfView.handleRightPos.bottom);
//            } else {
//            }
        } else {
            onTapHandled = pdfView.callbacks.callOnTap(e);
        }
        if(pdfView.pdfFile == null){
            return true;
        }
        boolean linkTapped = checkLinkTapped(e.getX(), e.getY());
        if (!onTapHandled && !linkTapped) {
            ScrollHandle ps = pdfView.getScrollHandle();
            if (ps != null && !pdfView.documentFitsView()) {
                if (!ps.shown()) {
                    ps.show();
                } else {
                    ps.hide();
                }
            }
        }
        pdfView.performClick();
        return true;
    }

    public int getCharIdxAtPos(float x, float y, int tolFactor) {
        try {
            PdfFile pdfFile = pdfView.pdfFile;
            if (pdfFile == null) {
                return -1;
            }

            float mappedX = -pdfView.getCurrentXOffset() + x;
            float mappedY = -pdfView.getCurrentYOffset() + y;
            int page = pdfFile.getPageAtOffset(pdfView.isSwipeVertical() ? mappedY : mappedX, pdfView.getZoom());
            SizeF pageSize = pdfFile.getScaledPageSize(page, pdfView.getZoom());


            int pageIndex = pdfFile.documentPage(page);

            long pagePtr = pdfFile.pdfDocument.mNativePagesPtr.get(pageIndex);//if it will produce nullPointerException catch will return -1 ...
            Log.e("pageIndex", String.valueOf(pageIndex));
            long tid = prepareText();
            if (pdfView.isNotCurrentPage(tid)) {
                return -1;
            }
            if (tid != 0) {
                //int charIdx = pdfiumCore.nativeGetCharIndexAtPos(tid, posX, posY, 10.0, 10.0);
                int pageX = (int) pdfFile.getSecondaryPageOffset(page, pdfView.getZoom());
                int pageY = (int) pdfFile.getPageOffset(page, pdfView.getZoom());
                return pdfFile.pdfiumCore.getCharIndexAtCoord(pagePtr, pageSize.getWidth(), pageSize.getHeight(), tid
                        , Math.abs(mappedX - pageX), Math.abs(mappedY - pageY), 10.0 * tolFactor, 10.0 * tolFactor);


            }



      /*  if (pdfFile == null) {
            return 0;
        }
        pdfView.sCursorPos.set(mappedX, mappedY);

       // float mappedX = -pdfView.getCurrentXOffset() + x;
       // float mappedY = -pdfView.getCurrentYOffset() + y;
        int page = pdfFile.getPageAtOffset(pdfView.isSwipeVertical() ? mappedY : mappedX, pdfView.getZoom());
        int  pageX = (int) pdfView.pdfFile.getSecondaryPageOffset(page, pdfView.getZoom());
        int  pageY = (int) pdfView.pdfFile.getPageOffset(page, pdfView.getZoom());

        float curX= Math.abs(mappedX - pageX);
        float curY=Math.abs(mappedY - pageY);


        int pageIndex = pdfView.pdfFile.documentPage(page);
        long pagePtr = pdfView.pdfFile.pdfDocument.mNativePagesPtr.get(pageIndex);
        SizeF pageSize = pdfFile.getScaledPageSize(page, pdfView.getZoom());
        prepareText();
        if (tid != 0) {
            return pdfFile.pdfiumCore.nativeGetCharIndexAtCoord(pagePtr, pageSize.getWidth(), pageSize.getHeight(), tid
                    ,  curX , curY , 10, 10);

        }*/
        }catch (Exception e){
            return -1;
        }
        return -1;
    }

    public int getCharIdxAt(float x, float y, int tolFactor) {
        PdfFile pdfFile = pdfView.pdfFile;
        if (pdfFile == null) {
            return -1;
        }
        int page = pdfView.currentPage;
        SizeF pageSize = pdfFile.getPageSize(page);


        int pageIndex = pdfFile.documentPage(page);
        long pagePtr = pdfFile.pdfDocument.mNativePagesPtr.get(pageIndex);
        Log.e("pageIndex", String.valueOf(pageIndex));
        long tid = prepareText();
        if (pdfView.isNotCurrentPage(tid)) {
            return -1;
        }
        if (tid != 0) {
            //int charIdx = pdfiumCore.nativeGetCharIndexAtPos(tid, posX, posY, 10.0, 10.0);
            int pageX = (int) pdfFile.getSecondaryPageOffset(page, pdfView.getZoom());
            int pageY = (int) pdfFile.getPageOffset(page, pdfView.getZoom());
            return pdfFile.pdfiumCore.getCharIndexAtCoord(pagePtr, pageSize.getWidth(), pageSize.getHeight(), tid
                    , x, y, 10.0 * tolFactor, 10.0 * tolFactor);
        }



      /*  if (pdfFile == null) {
            return 0;
        }
        pdfView.sCursorPos.set(mappedX, mappedY);

       // float mappedX = -pdfView.getCurrentXOffset() + x;
       // float mappedY = -pdfView.getCurrentYOffset() + y;
        int page = pdfFile.getPageAtOffset(pdfView.isSwipeVertical() ? mappedY : mappedX, pdfView.getZoom());
        int  pageX = (int) pdfView.pdfFile.getSecondaryPageOffset(page, pdfView.getZoom());
        int  pageY = (int) pdfView.pdfFile.getPageOffset(page, pdfView.getZoom());

        float curX= Math.abs(mappedX - pageX);
        float curY=Math.abs(mappedY - pageY);


        int pageIndex = pdfView.pdfFile.documentPage(page);
        long pagePtr = pdfView.pdfFile.pdfDocument.mNativePagesPtr.get(pageIndex);
        SizeF pageSize = pdfFile.getScaledPageSize(page, pdfView.getZoom());
        prepareText();
        if (tid != 0) {
            return pdfFile.pdfiumCore.nativeGetCharIndexAtCoord(pagePtr, pageSize.getWidth(), pageSize.getHeight(), tid
                    ,  curX , curY , 10, 10);

        }*/
        return -1;
    }

    private boolean wordTapped(float x, float y, float tolFactor) {
        PdfFile pdfFile = pdfView.pdfFile;
        if (pdfFile == null) {
            return false;
        }

        try {
            float mappedX = -pdfView.getCurrentXOffset() + x;
            float mappedY = -pdfView.getCurrentYOffset() + y;
            int page = pdfFile.getPageAtOffset(pdfView.isSwipeVertical() ? mappedY : mappedX, pdfView.getZoom());
            SizeF pageSize = pdfFile.getScaledPageSize(page, pdfView.getZoom());


            int pageIndex = pdfFile.documentPage(page);
            if (pdfFile.pdfDocument.hasPage(pageIndex) && pdfFile.pdfDocument.mNativePagesPtr.size() > 0) {
                long pagePtr = pdfFile.pdfDocument.mNativePagesPtr.get(pageIndex);
                long tid = prepareText();
                currentTextPtr = tid;
                if (tid != 0) {
                    //int charIdx = pdfiumCore.nativeGetCharIndexAtPos(tid, posX, posY, 10.0, 10.0);
                    int pageX = (int) pdfFile.getSecondaryPageOffset(page, pdfView.getZoom());
                    int pageY = (int) pdfFile.getPageOffset(page, pdfView.getZoom());
                    int charIdx = pdfFile.pdfiumCore.getCharIndexAtCoord(pagePtr, pageSize.getWidth(), pageSize.getHeight(), tid
                            , Math.abs(mappedX - pageX), Math.abs(mappedY - pageY), 10.0 * tolFactor, 10.0 * tolFactor);
                    String ret = null;

                    if (charIdx >= 0) {
                        int ed = pageBreakIterator.following(charIdx);
                        int st = pageBreakIterator.previous();
                        try {
                            ret = allText.substring(st, ed);
                            pdfView.setSelectionAtPage(pageIndex, st, ed);
//                    Toast.makeText(pdfView.getContext(), String.valueOf(ret), Toast.LENGTH_SHORT).show();
                            return true;
                        } catch (Exception e) {
                            e.printStackTrace();
                        }
                    }
                }
            }

        /*for (PdfDocument.Link link : pdfFile.getPageLinks(page)) {
            RectF mapped = pdfFile.mapRectToDevice(page, pageX, pageY, (int) pageSize.getWidth(),
                    (int) pageSize.getHeight(), link.getBounds());
            mapped.sort();
            if (mapped.contains(mappedX, mappedY)) {
                pdfView.callbacks.callLinkHandler(new LinkTapEvent(x, y, mappedX, mappedY, mapped, link));
                return true;
            }
        }*/
        } catch (Exception e) {
            e.printStackTrace();
        }
        return false;
    }

    public void getSelRects(ArrayList<RectF> rectPagePool, int selSt, int selEd) {
        float mappedX = -pdfView.getCurrentXOffset() + lastX;
        float mappedY = -pdfView.getCurrentYOffset() + lastY;
        int page = pdfView.pdfFile.getPageAtOffset(pdfView.isSwipeVertical() ? mappedY : mappedX, pdfView.getZoom());

        //CMN.Log("getTextRects", selSt, selEd);

        long tid = prepareText();
        if (pdfView.isNotCurrentPage(tid)) {
            return;
        }
        rectPagePool.clear();
        if (tid != 0) {
            if (selEd == -1) {
                selEd = allText.length();
            }

            if (selEd < selSt) {
                int tmp = selSt;
                selSt = selEd;
                selEd = tmp;
            }
            selEd -= selSt;
            if (selEd > 0) {


                long pagePtr = pdfView.pdfFile.pdfDocument.mNativePagesPtr.get(page);
                int pageX = (int) pdfView.pdfFile.getSecondaryPageOffset(page, pdfView.getZoom());
                int pageY = (int) pdfView.pdfFile.getPageOffset(page, pdfView.getZoom());
                pdfView.pdfiumCore.getPageSize(pdfView.pdfFile.pdfDocument, page);
                SizeF size = pdfView.pdfFile.getPageSize(page);
                int rectCount = pdfView.pdfiumCore.getTextRects(pagePtr
                        , 0
                        , 0
                        , new Size((int) size.getWidth(), (int) size.getHeight()), rectPagePool, tid, selSt, selEd);
                Log.e("getTextRects", selSt + "$" + selEd + "$" + rectCount + "$" + rectPagePool.toString());
                if (rectCount >= 0 && rectPagePool.size() > rectCount) {
                    rectPagePool.subList(rectCount, rectPagePool.size()).clear();
                }

            }
        }
    }

    private boolean checkLinkTapped(float x, float y) {
        PdfFile pdfFile = pdfView.pdfFile;
        if (pdfFile == null) {
            return false;
        }
        float mappedX = -pdfView.getCurrentXOffset() + x;
        float mappedY = -pdfView.getCurrentYOffset() + y;
        int page = pdfFile.getPageAtOffset(pdfView.isSwipeVertical() ? mappedY : mappedX, pdfView.getZoom());
        SizeF pageSize = pdfFile.getScaledPageSize(page, pdfView.getZoom());
        int pageX, pageY;
        if (pdfView.isSwipeVertical()) {
            pageX = (int) pdfFile.getSecondaryPageOffset(page, pdfView.getZoom());
            pageY = (int) pdfFile.getPageOffset(page, pdfView.getZoom());
        } else {
            pageY = (int) pdfFile.getSecondaryPageOffset(page, pdfView.getZoom());
            pageX = (int) pdfFile.getPageOffset(page, pdfView.getZoom());
        }
        for (PdfDocument.Link link : pdfFile.getPageLinks(page)) {
            RectF mapped = pdfFile.mapRectToDevice(page, pageX, pageY, (int) pageSize.getWidth(),
                    (int) pageSize.getHeight(), link.getBounds());
            mapped.sort();
            if (mapped.contains(mappedX, mappedY)) {
                pdfView.callbacks.callLinkHandler(new LinkTapEvent(x, y, mappedX, mappedY, mapped, link));
                return true;
            }
        }
        return false;
    }

    public long prepareText() {
        float mappedX = -pdfView.getCurrentXOffset() + lastX;
        float mappedY = -pdfView.getCurrentYOffset() + lastY;
        if (pdfView.pdfFile == null) return 0L;
        int page = pdfView.pdfFile.getPageAtOffset(pdfView.isSwipeVertical() ? mappedY : mappedX, pdfView.getZoom());
        return prepareText(page);

    }

    public long prepareText(int page) {
        long tid = loadText(page);
        if (tid != -1) {
            allText = pdfView.pdfiumCore.getText(tid);

            if (pageBreakIterator == null) {
                pageBreakIterator = new BreakIteratorHelper();
            }
            pageBreakIterator.setText(allText);
        }
        return tid;
    }

    public Long loadText() {

        float mappedX = -pdfView.getCurrentXOffset() + lastX;
        float mappedY = -pdfView.getCurrentYOffset() + lastY;
        if (pdfView.pdfFile == null) return 0L;
        int page = pdfView.pdfFile.getPageAtOffset(pdfView.isSwipeVertical() ? mappedY : mappedX, pdfView.getZoom());

        return loadText(page);

    }

    public Long loadText(int page) {
        try {
            if (pdfView.pdfFile == null) return 0L;
            synchronized (lock) {
                if (!pdfView.pdfFile.pdfDocument.hasPage(page)) {
                    try {
                        pdfView.pdfFile.openPage(page);
                    } catch (PageRenderingException e) {
                        e.printStackTrace();
                    }


                }
                long pagePtr = pdfView.pdfFile.pdfDocument.mNativePagesPtr.get(page);//if anyhow, it will produce null pointer exception ...catch statement will return 0 instead of crash
                if (!pdfView.pdfFile.pdfDocument.hasText(page)) {
                    long openTextPtr = pdfView.pdfiumCore.openText(pagePtr);
                    pdfView.pdfFile.pdfDocument.mNativeTextPtr.put(page, openTextPtr);
                }
            }
            return pdfView.pdfFile.pdfDocument.mNativeTextPtr.get(page);
        }catch (Exception e){
            return 0L;
        }
    }

    private void startPageFling(MotionEvent downEvent, MotionEvent ev, float velocityX, float velocityY) {
        if (!checkDoPageFling(velocityX, velocityY)) {
            return;
        }

        int direction;
        if (pdfView.isSwipeVertical()) {
            direction = velocityY > 0 ? -1 : 1;
        } else {
            direction = velocityX > 0 ? -1 : 1;
        }
        // get the focused page during the down event to ensure only a single page is changed
        float delta = pdfView.isSwipeVertical() ? ev.getY() - downEvent.getY() : ev.getX() - downEvent.getX();
        float offsetX = pdfView.getCurrentXOffset() - delta * pdfView.getZoom();
        float offsetY = pdfView.getCurrentYOffset() - delta * pdfView.getZoom();
        int startingPage = pdfView.findFocusPage(offsetX, offsetY);
        int targetPage = Math.max(0, Math.min(pdfView.getPageCount() - 1, startingPage + direction));

        SnapEdge edge = pdfView.findSnapEdge(targetPage);
        float offset = pdfView.snapOffsetForPage(targetPage, edge);
        animationManager.startPageFlingAnimation(-offset);
    }

    @Override
    public boolean onDoubleTap(MotionEvent e) {
        if (!pdfView.isDoubletapEnabled()) {
            return false;
        }

        if (pdfView.getZoom() < pdfView.getMidZoom()) {
            pdfView.zoomWithAnimation(e.getX(), e.getY(), pdfView.getMidZoom());
        } else if (pdfView.getZoom() < pdfView.getMaxZoom()) {
            pdfView.zoomWithAnimation(e.getX(), e.getY(), pdfView.getMaxZoom());
        } else {
            pdfView.resetZoomWithAnimation();
        }
        return true;
    }

    @Override
    public boolean onDoubleTapEvent(MotionEvent e) {
        return false;
    }

    @Override
    public boolean onDown(MotionEvent e) {
        animationManager.stopFling();
        return true;
    }

    @Override
    public void onShowPress(MotionEvent e) {

    }

    @Override
    public boolean onSingleTapUp(MotionEvent e) {
        return false;
    }

    float scrollValue = 0;

    @Override
    public boolean onScroll(MotionEvent e1, MotionEvent e2, float distanceX, float distanceY) {
        if(pdfView.startInDrag){
            if (pdfView.hideView != null)
                pdfView.hideView.setVisibility(View.GONE);
        }else{
            if (pdfView.hideView != null)
                pdfView.hideView.setVisibility(View.VISIBLE);
        }

        if (pdfView.startInDrag)
            return true;
        scrolling = true;
        if (pdfView.isZooming() || pdfView.isSwipeEnabled()) {
            pdfView.moveRelativeTo(-distanceX, -distanceY);
        }
        if (!scaling || pdfView.doRenderDuringScale()) {
            pdfView.loadPageByOffset();
        }

        scrollValue = distanceY;
//        scrollValue =  Math.abs(scrollValue);
//        if (distanceY >= 0 && pdfView.getCurrentPage() == 0) {
//            // code to hide
//            if (pdfView.hideView != null)
//                pdfView.hideView.setVisibility(View.GONE);
//        }if (distanceY <= -10 && pdfView.getCurrentPage() == 0) {
//            // code to show
//            if (pdfView.hideView != null)
//                pdfView.hideView.setVisibility(View.VISIBLE);
//        }
        Log.e("ScrollY", String.valueOf(distanceY));
        return true;
    }

    private void onScrollEnd(MotionEvent event) {
        pdfView.loadPages();
        hideHandle();
        if (!animationManager.isFlinging()) {
            pdfView.performPageSnap();
        }
        if (scrollValue <= -10 && pdfView.getCurrentPage() == 0) {
            // code to show
            if (pdfView.hideView != null)
                pdfView.hideView.setVisibility(View.VISIBLE);
        }
    }

    @Override
    public void onLongPress(MotionEvent e) {
        if (pdfView.hasSelection) {
            pdfView.clearSelection();
        }
        if (wordTapped(e.getX(), e.getY(), 1.5f)) {
            pdfView.hasSelection = true;
            if (pdfView.onSelection != null) {
                pdfView.onSelection.onSelection(true);
            }
            draggingHandle = pdfView.handleRight;
            sCursorPosStart.set(pdfView.handleRightPos.right, pdfView.handleRightPos.bottom);
        }
        pdfView.callbacks.callOnLongPress(e);
    }

    @Override
    public boolean onFling(MotionEvent e1, MotionEvent e2, float velocityX, float velocityY) {
        if (!pdfView.isSwipeEnabled()) {
            return false;
        }
        if (pdfView.isPageFlingEnabled()) {
            if (pdfView.pageFillsScreen()) {
                onBoundedFling(velocityX, velocityY);
            } else {
                startPageFling(e1, e2, velocityX, velocityY);
            }
            return true;
        }

        int xOffset = (int) pdfView.getCurrentXOffset();
        int yOffset = (int) pdfView.getCurrentYOffset();

        float minX, minY;
        PdfFile pdfFile = pdfView.pdfFile;
        if (pdfView.isSwipeVertical()) {
            minX = -(pdfView.toCurrentScale(pdfFile.getMaxPageWidth()) - pdfView.getWidth());
            minY = -(pdfFile.getDocLen(pdfView.getZoom()) - pdfView.getHeight());
        } else {
            minX = -(pdfFile.getDocLen(pdfView.getZoom()) - pdfView.getWidth());
            minY = -(pdfView.toCurrentScale(pdfFile.getMaxPageHeight()) - pdfView.getHeight());
        }

        animationManager.startFlingAnimation(xOffset, yOffset, (int) (velocityX), (int) (velocityY),
                (int) minX, 0, (int) minY, 0);

        return true;
    }

    private void onBoundedFling(float velocityX, float velocityY) {
        int xOffset = (int) pdfView.getCurrentXOffset();
        int yOffset = (int) pdfView.getCurrentYOffset();

        PdfFile pdfFile = pdfView.pdfFile;

        float mappedX = -pdfView.getCurrentXOffset() + lastX;
        float mappedY = -pdfView.getCurrentYOffset() + lastY;
        int page = pdfFile.getPageAtOffset(pdfView.isSwipeVertical() ? mappedY : mappedX, pdfView.getZoom());

        float pageStart = -pdfFile.getPageOffset(page, pdfView.getZoom());
        float pageEnd = pageStart - pdfFile.getPageLength(page, pdfView.getZoom());
        float minX, minY, maxX, maxY;
        if (pdfView.isSwipeVertical()) {
            minX = -(pdfView.toCurrentScale(pdfFile.getMaxPageWidth()) - pdfView.getWidth());
            minY = pageEnd + pdfView.getHeight();
            maxX = 0;
            maxY = pageStart;
        } else {
            minX = pageEnd + pdfView.getWidth();
            minY = -(pdfView.toCurrentScale(pdfFile.getMaxPageHeight()) - pdfView.getHeight());
            maxX = pageStart;
            maxY = 0;
        }

        animationManager.startFlingAnimation(xOffset, yOffset, (int) (velocityX), (int) (velocityY),
                (int) minX, (int) maxX, (int) minY, (int) maxY);
    }

    @Override
    public boolean onScale(ScaleGestureDetector detector) {
        float dr = detector.getScaleFactor();
        float wantedZoom = pdfView.getZoom() * dr;
        float minZoom = Math.min(MINIMUM_ZOOM, pdfView.getMinZoom());
        float maxZoom = Math.min(MAXIMUM_ZOOM, pdfView.getMaxZoom());
        if (wantedZoom < minZoom) {
            dr = minZoom / pdfView.getZoom();
        } else if (wantedZoom > maxZoom) {
            dr = maxZoom / pdfView.getZoom();
        }
        pdfView.zoomCenteredRelativeTo(dr, new PointF(detector.getFocusX(), detector.getFocusY()));

        return true;
    }

    @Override
    public boolean onScaleBegin(ScaleGestureDetector detector) {
        scaling = true;
        return true;
    }

    @Override
    public void onScaleEnd(ScaleGestureDetector detector) {
        pdfView.loadPages();
        hideHandle();
        scaling = false;
    }

    @Override
    public boolean onTouch(View v, MotionEvent event) {
        if (!enabled) {
            return false;
        }

        boolean retVal = scaleGestureDetector.onTouchEvent(event);
        retVal = gestureDetector.onTouchEvent(event) || retVal;

        lastX = event.getX();
        lastY = event.getY();
        pdfView.redrawSel();
        if (event.getAction() == MotionEvent.ACTION_UP) {
            if (draggingHandle != null) {
                draggingHandle = null;
            }
            pdfView.startInDrag = false;
            if (scrolling) {
                scrolling = false;
                onScrollEnd(event);
            }
        } else if (event.getAction() == MotionEvent.ACTION_DOWN) {
            orgX = view_pager_toguard_lastX = lastX;
            orgY = view_pager_toguard_lastY = lastY;


            if (pdfView.hasSelection) {
                if (pdfView.handleLeft.getBounds().contains((int) orgX, (int) orgY)) {

                    draggingHandle = pdfView.handleLeft;
                    sCursorPosStart.set(pdfView.handleLeftPos.left, pdfView.handleLeftPos.bottom);
                } else if (pdfView.handleRight.getBounds().contains((int) orgX, (int) orgY)) {

                    draggingHandle = pdfView.handleRight;
                    sCursorPosStart.set(pdfView.handleRightPos.right, pdfView.handleRightPos.bottom);
                }
            }

        } else if (event.getAction() == MotionEvent.ACTION_MOVE) {


            dragHandle(event.getX(),
                    event.getY());
            view_pager_toguard_lastX = lastX;
            view_pager_toguard_lastY = lastY;
        }
        return true;
    }


    private void dragHandle(float x, float y) {
        if (draggingHandle != null) {
            pdfView.startInDrag = true;
            lineHeight = draggingHandle == pdfView.handleLeft ? pdfView.lineHeightLeft : pdfView.lineHeightRight;
            float posX = sCursorPosStart.x + (lastX - orgX) / pdfView.getZoom();
            float posY = sCursorPosStart.y + (lastY - orgY) / pdfView.getZoom();
            pdfView.sCursorPos.set(posX, posY);


            boolean isLeft = draggingHandle == pdfView.handleLeft;
            float mappedX = -pdfView.getCurrentXOffset() + x;
            float mappedY = -pdfView.getCurrentYOffset() + y;
            int page = pdfView.pdfFile.getPageAtOffset(pdfView.isSwipeVertical() ? mappedY : mappedX, pdfView.getZoom());


            int pageIndex = pdfView.pdfFile.documentPage(page);

            int charIdx = -1;
            int pageIdx = pageIndex;

            PDFView pageI = pdfView;
            // long pagePtr = pageI.pdfFile.pdfDocument.mNativePagesPtr.get(pageI.getCurrentPage());


            //posY -= pageI.getCurrentXOffset();
            //posX -= pageI.getCurrentXOffset();
            charIdx = getCharIdxAtPos(x, y - lineHeight, 10);
            pdfView.selectionPaintView.supressRecalcInval = true;
            Log.e("charIdx", String.valueOf(charIdx));
            if (charIdx >= 0) {
                if (isLeft) {
                    if (pageIdx != pdfView.selPageSt || charIdx != pdfView.selStart) {
                        pdfView.selPageSt = pageIdx;
                        pdfView.selStart = charIdx;
                        pdfView.selectionPaintView.resetSel();
                    }
                } else {
                    charIdx += 1;
                    if (pageIdx != pdfView.selPageEd || charIdx != pdfView.selEnd) {
                        pdfView.selPageEd = pageIdx;
                        pdfView.selEnd = charIdx;
                        pdfView.selectionPaintView.resetSel();
                    }
                }
            }
            pdfView.redrawSel();
            // Toast.makeText(pdfView.getContext(),   pdfView. getSelection(), Toast.LENGTH_SHORT).show();
            //   pdfView.text.setText(pdfView.getSelection());
            pdfView.selectionPaintView.supressRecalcInval = false;
        }
    }

    private void hideHandle() {
        ScrollHandle scrollHandle = pdfView.getScrollHandle();
        if (scrollHandle != null && scrollHandle.shown()) {
            scrollHandle.hideDelayed();
        }
    }

    private boolean checkDoPageFling(float velocityX, float velocityY) {
        float absX = Math.abs(velocityX);
        float absY = Math.abs(velocityY);
        return pdfView.isSwipeVertical() ? absY > absX : absX > absY;
    }
}
