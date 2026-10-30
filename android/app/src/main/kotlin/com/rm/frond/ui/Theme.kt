package com.rm.frond.ui

import androidx.compose.foundation.IndicationNodeFactory
import androidx.compose.foundation.LocalIndication
import androidx.compose.foundation.interaction.FocusInteraction
import androidx.compose.foundation.interaction.InteractionSource
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.material3.ripple
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.ContentDrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.node.DelegatableNode
import androidx.compose.ui.node.DelegatingNode
import androidx.compose.ui.node.DrawModifierNode
import androidx.compose.ui.node.invalidateDraw
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.launch

// Frond's palette: a dark frond green, palm-leaf green and the soft grey-green of a Palm LCD,
// with Palm OS title-bar blue as the second colour.
object Frond {
    val ink = Color(0xFF1E2A22)
    val leaf = Color(0xFF9CCB8E)
    val leafDeep = Color(0xFF356B3A)
    val lcd = Color(0xFFE9ECE2)
    val palmBlue = Color(0xFF3A5A94)
    val palmBlueLight = Color(0xFFAFC2E8)
}

private val darkScheme = darkColorScheme(
    primary = Frond.leaf,
    onPrimary = Frond.ink,
    primaryContainer = Color(0xFF2C4A2E),
    onPrimaryContainer = Color(0xFFC9E8BE),
    secondary = Frond.palmBlueLight,
    onSecondary = Color(0xFF13233F),
    secondaryContainer = Color(0xFF2A3A57),
    onSecondaryContainer = Color(0xFFD7E1F5),
    tertiary = Frond.lcd,
    onTertiary = Frond.ink,
    background = Color(0xFF151D17),
    onBackground = Frond.lcd,
    surface = Color(0xFF151D17),
    onSurface = Frond.lcd,
    surfaceVariant = Color(0xFF243127),
    onSurfaceVariant = Color(0xFFB4BCB0),
    surfaceContainerLowest = Color(0xFF0F1611),
    surfaceContainerLow = Color(0xFF19221B),
    surfaceContainer = Frond.ink,
    surfaceContainerHigh = Color(0xFF243127),
    surfaceContainerHighest = Color(0xFF2C3A2F),
    inverseSurface = Frond.lcd,
    inverseOnSurface = Frond.ink,
    inversePrimary = Frond.leafDeep,
    outline = Color(0xFF526055),
    outlineVariant = Color(0xFF2F3D32),
    error = Color(0xFFFFB4AB),
    onError = Color(0xFF690005),
)

private val lightScheme = lightColorScheme(
    primary = Frond.leafDeep,
    onPrimary = Color(0xFFFFFFFF),
    primaryContainer = Color(0xFFC9E8BE),
    onPrimaryContainer = Frond.ink,
    secondary = Frond.palmBlue,
    onSecondary = Color(0xFFFFFFFF),
    secondaryContainer = Color(0xFFD7E1F5),
    onSecondaryContainer = Color(0xFF13233F),
    tertiary = Frond.ink,
    onTertiary = Frond.lcd,
    background = Color(0xFFF1F3EC),
    onBackground = Frond.ink,
    surface = Color(0xFFF1F3EC),
    onSurface = Frond.ink,
    surfaceVariant = Color(0xFFDFE4D8),
    onSurfaceVariant = Color(0xFF475247),
    surfaceContainerLowest = Color(0xFFFBFCF7),
    surfaceContainerLow = Color(0xFFEBEEE5),
    surfaceContainer = Color(0xFFE5E9DE),
    surfaceContainerHigh = Color(0xFFDFE4D8),
    surfaceContainerHighest = Color(0xFFD8DED0),
    inverseSurface = Frond.ink,
    inverseOnSurface = Frond.lcd,
    inversePrimary = Frond.leaf,
    outline = Color(0xFF737E72),
    outlineVariant = Color(0xFFC4CBBE),
    error = Color(0xFFBA1A1A),
)

private val shapes = Shapes(
    extraSmall = RoundedCornerShape(2.dp),
    small = RoundedCornerShape(2.dp),
    medium = RoundedCornerShape(4.dp),
    large = RoundedCornerShape(6.dp),
    extraLarge = RoundedCornerShape(8.dp),
)

// Titles and labels in the system monospace, the rest in the default sans.
private val typography = Typography().let { t ->
    val mono = FontFamily.Monospace
    t.copy(
        headlineSmall = t.headlineSmall.copy(fontFamily = mono, fontSize = 22.sp),
        titleLarge = t.titleLarge.copy(fontFamily = mono, fontSize = 21.sp),
        titleMedium = t.titleMedium.copy(fontFamily = mono),
        titleSmall = t.titleSmall.copy(fontFamily = mono),
        labelLarge = t.labelLarge.copy(fontFamily = mono),
    )
}

// Follows the system's light or dark setting.
@Composable
fun FrondTheme(dark: Boolean = isSystemInDarkTheme(), content: @Composable () -> Unit) {
    val scheme = if (dark) darkScheme else lightScheme
    MaterialTheme(colorScheme = scheme, shapes = shapes, typography = typography) {
        // Everything pressable keeps its ripple, and shows a ring when the keys have it.
        CompositionLocalProvider(LocalIndication provides FocusRing(scheme.primary), content = content)
    }
}

// The ripple on a press, and a ring in [colour] round whatever the keyboard has focused.
private class FocusRing(private val colour: Color) : IndicationNodeFactory {
    override fun create(interactionSource: InteractionSource): DelegatableNode = FocusRingNode(interactionSource, colour)
    override fun equals(other: Any?) = other is FocusRing && other.colour == colour
    override fun hashCode() = colour.hashCode()
}

private class FocusRingNode(private val source: InteractionSource, private val colour: Color) : DelegatingNode(), DrawModifierNode {
    private var focused = false

    init {
        delegate(ripple().create(source))
    }

    override fun onAttach() {
        coroutineScope.launch {
            source.interactions.collect {
                when (it) {
                    is FocusInteraction.Focus -> focused = true
                    is FocusInteraction.Unfocus -> focused = false
                    else -> return@collect
                }
                invalidateDraw()
            }
        }
    }

    override fun ContentDrawScope.draw() {
        drawContent()
        if (!focused) return
        val w = RING.toPx()
        drawRoundRect(colour, Offset(w / 2, w / 2), Size(size.width - w, size.height - w), CornerRadius(CORNER.toPx()), style = Stroke(w))
    }

    private companion object {
        val RING = 3.dp
        val CORNER = 4.dp
    }
}
