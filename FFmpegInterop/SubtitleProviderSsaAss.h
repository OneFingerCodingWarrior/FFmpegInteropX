#pragma once

#include "SubtitleProvider.h"

#include <string>
#include <codecvt>

namespace FFmpegInterop
{
	ref class SubtitleProviderSsaAss : SubtitleProvider
	{
	internal:
		SubtitleProviderSsaAss(FFmpegReader^ reader,
			AVFormatContext* avFormatCtx,
			AVCodecContext* avCodecCtx,
			FFmpegInteropConfig^ config,
			int index,
			CoreDispatcher^ dispatcher,
			AttachedFileHelper^ attachedFileHelper)
			: SubtitleProvider(reader, avFormatCtx, avCodecCtx, config, index, TimedMetadataKind::Subtitle, dispatcher)
		{
			this->attachedFileHelper = attachedFileHelper;
		}

		virtual HRESULT Initialize() override
		{
			auto hr = SubtitleProvider::Initialize();
			if (SUCCEEDED(hr))
			{
				ssaVersion = 4;
				if (m_pAvCodecCtx->subtitle_header && m_pAvCodecCtx->subtitle_header_size > 0)
				{
					auto str = std::string((char*)m_pAvCodecCtx->subtitle_header, m_pAvCodecCtx->subtitle_header_size);
					auto versionIndex = str.find("ScriptType: v4.00+");
					if (versionIndex != str.npos)
					{
						isAss = true;
					}
					else
					{
						versionIndex = str.find("ScriptType: v");
						if (versionIndex != str.npos && versionIndex + 13 < str.length())
						{
							auto version = str.at(versionIndex + 13) - '0';
							if (version > 0 && version < 9)
							{
								ssaVersion = version;
							}
						}
					}
					
					auto resx = str.find("\nPlayResX: ");
					auto resy = str.find("\nPlayResY: ");
					if (resx != str.npos && resy != str.npos)
					{
						int w, h;
						if (sscanf_s((char*)m_pAvCodecCtx->subtitle_header + resx, "\nPlayResX: %i\n", &w) == 1 &&
							sscanf_s((char*)m_pAvCodecCtx->subtitle_header + resy, "\nPlayResY: %i\n", &h) == 1)
						{
							width = w;
							height = h;
						}
					}

					if (isAss)
					{
						ReadStylesV4Plus(str);
					}
					else if (ssaVersion == 4)
					{
						ReadStylesV4(str);
					}
				}

				if (ssaVersion >= 3)
				{
					textIndex = 9;
				}
				else
				{
					textIndex = 8;
				}
			}

			return hr;
		}

		virtual IMediaCue^ CreateCue(AVPacket* packet, TimeSpan* position, TimeSpan *duration) override
		{
			AVSubtitle subtitle;
			int gotSubtitle = 0;
			auto result = avcodec_decode_subtitle2(m_pAvCodecCtx, &subtitle, &gotSubtitle, packet);
			if (result > 0 && gotSubtitle && subtitle.num_rects > 0)
			{
				auto str = utf8_to_wstring(std::string(subtitle.rects[0]->ass));
				
				int startStyle = -1;
				int endStyle = -1;
				int lastComma = -1;
				bool hasError = false;
				for (int i = 0; i < textIndex; i++)
				{
					auto nextComma = str.find(',', lastComma + 1);
					if (nextComma != str.npos)
					{
						if (i == styleIndex)
						{
							startStyle = (int)nextComma + 1;
						}
						else if (i == styleIndex + 1)
						{
							endStyle = (int)nextComma;
						}
						lastComma = (int)nextComma;
					}
					else
					{
						// this should not happen. still we try to be graceful. let's use what we found.
						hasError = true;
						break;
					}
				}

				SsaStyleDefinition^ style = nullptr;
				if (!hasError && startStyle > 0 && endStyle > 0)
				{
					auto styleName = convertFromString(str.substr(startStyle, endStyle - startStyle));
					auto result = styles.find(styleName);
					if (result != styles.end())
					{
						style = result->second;
					}
				}

				if (lastComma > 0 && lastComma < (int)str.length() - 1)
				{
					// get actual text
					str = str.substr(lastComma + 1);

					find_and_replace(str, L"\\N", L"\n");
					find_and_replace(str, L"\\h", L"\t");
					str.erase(str.find_last_not_of(L" \n\r") + 1);

					auto cueStyle = !m_config->OverrideSubtitleStyles && style ? style->Style : m_config->SubtitleStyle;
					auto cueRegion = !m_config->OverrideSubtitleStyles && style ? style->Region : m_config->SubtitleRegion;

					TimedTextCue^ cue = ref new TimedTextCue();
					cue->CueRegion = cueRegion;
					cue->CueStyle = cueStyle;

					TimedTextLine^ textLine = ref new TimedTextLine();
					cue->Lines->Append(textLine);

					TimedTextStyle^ subStyle = nullptr;
					TimedTextSubformat^ subFormat = nullptr;
					TimedTextRegion^ subRegion = nullptr;
					TimedTextStyle^ subCueStyle = nullptr;
					while (true)
					{
						auto nextEffect = str.find('{');
						if (nextEffect != str.npos)
						{
							auto endEffect = str.find('}', nextEffect);
							if (endEffect != str.npos && endEffect - nextEffect > 2)
							{
								// create a subformat with default style for beginning of text (UWP seems to need subformats for all text, if used)
								if (nextEffect > 0 && subFormat == nullptr)
								{
									subFormat = ref new TimedTextSubformat();
									subFormat->SubformatStyle = cueStyle;
									subFormat->StartIndex = 0;
								}
							
								// apply previous subformat, if any
								if (subFormat != nullptr)
								{
									subFormat->Length = nextEffect - subFormat->StartIndex;
									textLine->Subformats->Append(subFormat);
								}

								// create new subformat for following text
								subStyle = subStyle != nullptr ? CopyStyle(subStyle) : CopyStyle(cueStyle);
								subFormat = ref new TimedTextSubformat();
								subFormat->SubformatStyle = subStyle;
								subFormat->StartIndex = nextEffect;

								auto effectContent = str[nextEffect+1] == L'\\' ? str.substr(nextEffect + 2, endEffect - nextEffect - 2) : str.substr(nextEffect + 1, endEffect - nextEffect - 1);
								std::vector<std::wstring> tags;
								auto start = 0U;
								auto end = effectContent.find(L"\\");
								while (end != std::string::npos)
								{
									tags.push_back(effectContent.substr(start, end - start));
									start = end + 1;
									end = effectContent.find(L"\\", start);
								}
								tags.push_back(effectContent.substr(start, end));

								for each (auto tag in tags)
								{
									try
									{
										if (startsWith(tag, L"fn"))
										{
											auto fnName = tag.substr(2);
											subStyle->FontFamily = GetFontFamily(fnName);
										}
										else if (startsWith(tag, L"fs"))
										{
											auto size = parseDouble(tag.substr(2));
											TimedTextDouble fontSize;
											fontSize.Unit = TimedTextUnit::Pixels;
											fontSize.Value = size;
											subStyle->FontSize = fontSize;
										}
										else if (startsWith(tag, L"c"))
										{
											int color = parseHexInt(tag.substr(3, 6));
											subStyle->Foreground = ColorFromArgb(color << 8 | 0x000000FF);
										}
										else if (startsWith(tag, L"1c"))
										{
											int color = parseHexInt(tag.substr(4, 6));
											subStyle->Foreground = ColorFromArgb(color << 8 | 0x000000FF);
										}
										else if (startsWith(tag, L"3c"))
										{
											int color = parseHexInt(tag.substr(4, 6));
											subStyle->OutlineColor = ColorFromArgb(color << 8 | 0x000000FF);
										}
										else if (startsWith(tag, L"alpha"))
										{
										}
										else if (startsWith(tag, L"an"))
										{
											// legacy alignment, but I think we should support it
											auto alignment = parseInt(tag.substr(2));

											subRegion = subRegion != nullptr ? CopyRegion(subRegion) : CopyRegion(cueRegion);
											subRegion->DisplayAlignment = GetVerticalAlignment(alignment);
											subCueStyle = subCueStyle != nullptr ? CopyStyle(subCueStyle) : CopyStyle(cueStyle);
											subCueStyle->LineAlignment = GetHorizontalAlignment(alignment);
										}
										else if (startsWith(tag, L"a"))
										{
											auto alignment = parseInt(tag.substr(1));

											subRegion = subRegion != nullptr ? CopyRegion(subRegion) : CopyRegion(cueRegion);
											subRegion->DisplayAlignment = GetVerticalAlignment(alignment);
											subCueStyle = subCueStyle != nullptr ? CopyStyle(subCueStyle) : CopyStyle(cueStyle);
											subCueStyle->LineAlignment = GetHorizontalAlignment(alignment);
										}
										else if (tag.compare(L"b0") == 0)
										{
											subStyle->FontWeight = TimedTextWeight::Normal;
										}
										else if (tag.compare(L"b1") == 0)
										{
											subStyle->FontWeight = TimedTextWeight::Bold;
										}
										else if (tag.compare(L"i0") == 0 && WFM::ApiInformation::IsPropertyPresent("Windows.Media.Core.TimedTextStyle", "FontStyle"))
										{
											subStyle->FontStyle = TimedTextFontStyle::Normal;
										}
										else if (tag.compare(L"i1") == 0 && WFM::ApiInformation::IsPropertyPresent("Windows.Media.Core.TimedTextStyle", "FontStyle"))
										{
											subStyle->FontStyle = TimedTextFontStyle::Italic;
										}
										else if (tag.compare(L"u0") == 0 && WFM::ApiInformation::IsPropertyPresent("Windows.Media.Core.TimedTextStyle", "IsUnderlineEnabled"))
										{
											subStyle->IsUnderlineEnabled = false;
										}
										else if (tag.compare(L"u1") == 0 && WFM::ApiInformation::IsPropertyPresent("Windows.Media.Core.TimedTextStyle", "IsUnderlineEnabled"))
										{
											subStyle->IsUnderlineEnabled = true;
										}
										else if (tag.compare(L"s0") == 0 && WFM::ApiInformation::IsPropertyPresent("Windows.Media.Core.TimedTextStyle", "IsLineThroughEnabled"))
										{
											subStyle->IsLineThroughEnabled = false;
										}
										else if (tag.compare(L"s1") == 0 && WFM::ApiInformation::IsPropertyPresent("Windows.Media.Core.TimedTextStyle", "IsLineThroughEnabled"))
										{
											subStyle->IsLineThroughEnabled = true;
										}
										else if (startsWith(tag, L"fn"))
										{
											auto fnName = tag.substr(2);
											subStyle->FontFamily = convertFromString(fnName);
										}
										else if (startsWith(tag, L"fn"))
										{
											auto fnName = tag.substr(2);
											subStyle->FontFamily = convertFromString(fnName);
										}
										else if (startsWith(tag, L"fn"))
										{
											auto fnName = tag.substr(2);
											subStyle->FontFamily = convertFromString(fnName);
										}
									}
									catch (...)
									{
										OutputDebugString(L"Failed to parse tag: ");
									}
								}

								// strip effect from actual text
								if (endEffect < str.length() - 1)
								{
									str = str.substr(0, nextEffect).append(str.substr(endEffect + 1));
								}
								else
								{
									str = str.substr(0, nextEffect);
								}
							}
							else
							{
								break;
							}
						}
						else
						{
							break;
						}
					}

					// apply last subformat, if any
					if (subFormat != nullptr)
					{
						subFormat->Length = str.size() - subFormat->StartIndex;
						textLine->Subformats->Append(subFormat);
					}
					if (subRegion != nullptr)
					{
						cue->CueRegion = subRegion;
					}
					if (subCueStyle != nullptr)
					{
						cue->CueStyle = subCueStyle;
					}
					auto timedText = convertFromString(str);
					if (timedText->Length() > 0)
					{
						textLine->Text = timedText;

						return cue;
					}
				}
			}
			else if (result <= 0)
			{
				OutputDebugString(L"Failed to decode subtitle.");
			}

			return nullptr;
		}

		void ReadStylesV4Plus(std::string str)
		{
			auto stylesV4plus = str.find("[V4+ Styles]");
			while (stylesV4plus != str.npos)
			{
				stylesV4plus = str.find("\nStyle: ", stylesV4plus);
				if (stylesV4plus != str.npos)
				{
					stylesV4plus += 8;
					/*
					[V4+ Styles]
					Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
					*/
					const unsigned int MAX_STYLE_NAME_CHARS = 256;
					char name[MAX_STYLE_NAME_CHARS];
					char font[MAX_STYLE_NAME_CHARS];
					float size;
					int color, secondaryColor, outlineColor, backColor;
					int bold, italic, underline, strikeout;
					float scaleX, scaleY, spacing, angle;
					int borderStyle;
					float outline;
					int shadow, alignment;
					float marginL, marginR, marginV;
					int encoding;

					// try with hex colors
					auto count = sscanf_s((char*)m_pAvCodecCtx->subtitle_header + stylesV4plus,
						"%[^,],%[^,],%f,&H%x,&H%x,&H%x,&H%x,%i,%i,%i,%i,%f,%f,%f,%f,%i,%f,%i,%i,%f,%f,%f,%i",
						name, MAX_STYLE_NAME_CHARS, font, MAX_STYLE_NAME_CHARS,
						&size, &color, &secondaryColor, &outlineColor, &backColor,
						&bold, &italic, &underline, &strikeout,
						&scaleX, &scaleY, &spacing, &angle, &borderStyle,
						&outline, &shadow, &alignment,
						&marginL, &marginR, &marginV, &encoding);

					if (count == 3)
					{
						// try with decimal colors
						auto count = sscanf_s((char*)m_pAvCodecCtx->subtitle_header + stylesV4plus,
							"%[^,],%[^,],%f,%i,%i,%i,%i,%i,%i,%i,%i,%f,%f,%f,%f,%i,%f,%i,%i,%f,%f,%f,%i",
							name, MAX_STYLE_NAME_CHARS, font, MAX_STYLE_NAME_CHARS,
							&size, &color, &secondaryColor, &outlineColor, &backColor,
							&bold, &italic, &underline, &strikeout,
							&scaleX, &scaleY, &spacing, &angle, &borderStyle,
							&outline, &shadow, &alignment,
							&marginL, &marginR, &marginV, &encoding);
					}

					if (count == 23)
					{
						auto verticalAlignment = GetVerticalAlignment(alignment);

						auto horizontalAlignment = GetHorizontalAlignment(alignment);

						auto SubtitleRegion = ref new TimedTextRegion();

						TimedTextSize extent;
						extent.Unit = TimedTextUnit::Percentage;
						extent.Width = 100;
						extent.Height = 100;
						SubtitleRegion->Extent = extent;
						TimedTextPoint position;
						position.Unit = TimedTextUnit::Pixels;
						position.X = 0;
						position.Y = 0;
						SubtitleRegion->Position = position;
						SubtitleRegion->DisplayAlignment = verticalAlignment;
						SubtitleRegion->Background = Windows::UI::Colors::Transparent;
						SubtitleRegion->ScrollMode = TimedTextScrollMode::Rollup;
						SubtitleRegion->TextWrapping = TimedTextWrapping::Wrap;
						SubtitleRegion->WritingMode = TimedTextWritingMode::LeftRightTopBottom;
						SubtitleRegion->IsOverflowClipped = false;
						SubtitleRegion->ZIndex = 0;
						TimedTextDouble LineHeight;
						LineHeight.Unit = TimedTextUnit::Percentage;
						LineHeight.Value = 100;
						SubtitleRegion->LineHeight = LineHeight;
						TimedTextPadding padding;
						padding.Unit = TimedTextUnit::Percentage;
						padding.Start = 0;
						if (width > 0 && height > 0)
						{
							padding.Start = (double)marginL * 100 / width;
							padding.End = (double)marginR * 100 / width;
							padding.After = (double)marginV * 100 / height;
						}
						else
						{
							padding.After = 12;
						}
						SubtitleRegion->Padding = padding;
						SubtitleRegion->Name = "";

						auto SubtitleStyle = ref new TimedTextStyle();

						SubtitleStyle->FontFamily = GetFontFamily(font);
						TimedTextDouble fontSize;
						fontSize.Unit = TimedTextUnit::Pixels;
						fontSize.Value = size;
						SubtitleStyle->FontSize = fontSize;
						SubtitleStyle->LineAlignment = horizontalAlignment;
						if (Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent("Windows.Media.Core.TimedTextStyle", "FontStyle"))
						{
							SubtitleStyle->FontStyle = italic ? TimedTextFontStyle::Italic : TimedTextFontStyle::Normal;
						}
						SubtitleStyle->FontWeight = bold ? TimedTextWeight::Bold : TimedTextWeight::Normal;
						SubtitleStyle->Foreground = ColorFromArgb(color << 8 | 0x000000FF);
						SubtitleStyle->Background = Windows::UI::Colors::Transparent; //ColorFromArgb(backColor);
						TimedTextDouble outlineRadius;
						outlineRadius.Unit = TimedTextUnit::Percentage;
						outlineRadius.Value = outline * 2;
						SubtitleStyle->OutlineRadius = outlineRadius;
						TimedTextDouble outlineThickness;
						outlineThickness.Unit = TimedTextUnit::Percentage;
						outlineThickness.Value = outline * 2;
						SubtitleStyle->OutlineThickness = outlineThickness;
						SubtitleStyle->FlowDirection = TimedTextFlowDirection::LeftToRight;
						SubtitleStyle->OutlineColor = ColorFromArgb(outlineColor << 8 | 0x000000FF);

						if (Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent("Windows.Media.Core.TimedTextStyle", "IsUnderlineEnabled"))
						{
							SubtitleStyle->IsUnderlineEnabled = underline;
						}

						if (Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent("Windows.Media.Core.TimedTextStyle", "IsLineThroughEnabled"))
						{
							SubtitleStyle->IsLineThroughEnabled = strikeout;
						}

						auto style = ref new SsaStyleDefinition();
						auto wname = utf8_to_wstring(std::string(name));
						style->Name = convertFromString(wname);
						style->Region = SubtitleRegion;
						style->Style = SubtitleStyle;

						styles[style->Name] = style;
					}
				}
				else
				{
					break;
				}
			}
		}

		void ReadStylesV4(std::string str)
		{
			auto stylesV4plus = str.find("[V4 Styles]");
			while (stylesV4plus != str.npos)
			{
				stylesV4plus = str.find("\nStyle: ", stylesV4plus);
				if (stylesV4plus != str.npos)
				{
					stylesV4plus += 8;
					/*
					[V4+ Styles]
					Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
					[V4 Styles]
					Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, TertiaryColour, BackColour, Bold, Italic, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, AlphaLevel, Encoding
					*/
					const unsigned int MAX_STYLE_NAME_CHARS = 256;
					char name[MAX_STYLE_NAME_CHARS];
					char font[MAX_STYLE_NAME_CHARS];
					float size;
					int color, secondaryColor, outlineColor, backColor;
					int bold, italic, borderstyle;
					float outline;
					int shadow, alignment;
					float marginL, marginR, marginV, alpha;
					int encoding;

					auto count = sscanf_s((char*)m_pAvCodecCtx->subtitle_header + stylesV4plus,
						"%[^,],%[^,],%f,%i,%i,%i,%i,%i,%i,%i,%f,%i,%i,%f,%f,%f,%f,%i",
						name, MAX_STYLE_NAME_CHARS, font, MAX_STYLE_NAME_CHARS,
						&size, &color, &secondaryColor, &outlineColor, &backColor,
						&bold, &italic, &borderstyle,
						&outline, &shadow, &alignment,
						&marginL, &marginR, &marginV, &alpha, &encoding);

					if (count == 3)
					{
						// try with hex colors
						count = sscanf_s((char*)m_pAvCodecCtx->subtitle_header + stylesV4plus,
							"%[^,],%[^,],%f,&H%x,&H%x,&H%x,&H%x,%i,%i,%i,%f,%i,%i,%f,%f,%f,%f,%i",
							name, MAX_STYLE_NAME_CHARS, font, MAX_STYLE_NAME_CHARS,
							&size, &color, &secondaryColor, &outlineColor, &backColor,
							&bold, &italic, &borderstyle,
							&outline, &shadow, &alignment,
							&marginL, &marginR, &marginV, &alpha, &encoding);
					}

					if (count == 18)
					{
						auto verticalAlignment = GetVerticalAlignment(alignment);

						auto horizontalAlignment = GetHorizontalAlignment(alignment);

						auto SubtitleRegion = ref new TimedTextRegion();

						TimedTextSize extent;
						extent.Unit = TimedTextUnit::Percentage;
						extent.Width = 100;
						extent.Height = 100;
						SubtitleRegion->Extent = extent;
						TimedTextPoint position;
						position.Unit = TimedTextUnit::Pixels;
						position.X = 0;
						position.Y = 0;
						SubtitleRegion->Position = position;
						SubtitleRegion->DisplayAlignment = verticalAlignment;
						SubtitleRegion->Background = Windows::UI::Colors::Transparent;
						SubtitleRegion->ScrollMode = TimedTextScrollMode::Rollup;
						SubtitleRegion->TextWrapping = TimedTextWrapping::Wrap;
						SubtitleRegion->WritingMode = TimedTextWritingMode::LeftRightTopBottom;
						SubtitleRegion->IsOverflowClipped = false;
						SubtitleRegion->ZIndex = 0;
						TimedTextDouble LineHeight;
						LineHeight.Unit = TimedTextUnit::Percentage;
						LineHeight.Value = 100;
						SubtitleRegion->LineHeight = LineHeight;
						TimedTextPadding padding;
						padding.Unit = TimedTextUnit::Percentage;
						padding.Start = 0;
						if (width > 0 && height > 0)
						{
							padding.Start = (double)marginL * 100 / width;
							padding.End = (double)marginR * 100 / width;
							padding.After = (double)marginV * 100 / height;
						}
						else
						{
							padding.After = 12;
						}
						SubtitleRegion->Padding = padding;
						SubtitleRegion->Name = "";

						auto SubtitleStyle = ref new TimedTextStyle();

						SubtitleStyle->FontFamily = GetFontFamily(font);
						TimedTextDouble fontSize;
						fontSize.Unit = TimedTextUnit::Pixels;
						fontSize.Value = size;
						SubtitleStyle->FontSize = fontSize;
						SubtitleStyle->LineAlignment = horizontalAlignment;
						if (Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent("Windows.Media.Core.TimedTextStyle", "FontStyle"))
						{
							SubtitleStyle->FontStyle = italic ? TimedTextFontStyle::Italic : TimedTextFontStyle::Normal;
						}
						SubtitleStyle->FontWeight = bold ? TimedTextWeight::Bold : TimedTextWeight::Normal;
						SubtitleStyle->Foreground = ColorFromArgb(color << 8 | 0x000000FF);
						SubtitleStyle->Background = Windows::UI::Colors::Transparent; //ColorFromArgb(backColor);
						TimedTextDouble outlineRadius;
						outlineRadius.Unit = TimedTextUnit::Percentage;
						outlineRadius.Value = outline * 2;
						SubtitleStyle->OutlineRadius = outlineRadius;
						TimedTextDouble outlineThickness;
						outlineThickness.Unit = TimedTextUnit::Percentage;
						outlineThickness.Value = outline * 2;
						SubtitleStyle->OutlineThickness = outlineThickness;
						SubtitleStyle->FlowDirection = TimedTextFlowDirection::LeftToRight;
						SubtitleStyle->OutlineColor = ColorFromArgb(outlineColor << 8 | 0x000000FF);

						auto style = ref new SsaStyleDefinition();
						auto wname = utf8_to_wstring(std::string(name));
						style->Name = convertFromString(wname);
						style->Region = SubtitleRegion;
						style->Style = SubtitleStyle;

						styles[style->Name] = style;
					}
				}
				else
				{
					break;
				}
			}
		}

		TimedTextStyle^ CopyStyle(TimedTextStyle^ style)
		{
			auto copy = ref new TimedTextStyle();
			copy->Background = style->Background;
			copy->FlowDirection = style->FlowDirection;
			copy->FontFamily = style->FontFamily;
			copy->FontSize = style->FontSize;
			copy->FontWeight = style->FontWeight;
			copy->Foreground = style->Foreground;
			copy->OutlineColor = style->OutlineColor;
			copy->OutlineRadius = style->OutlineRadius;
			copy->OutlineThickness = style->OutlineThickness;

			if (Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent("Windows.Media.Core.TimedTextStyle", "FontStyle"))
			{
				copy->FontStyle = style->FontStyle;
			}
			if (Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent("Windows.Media.Core.TimedTextStyle", "IsLineThroughEnabled"))
			{
				copy->IsLineThroughEnabled = style->IsLineThroughEnabled;
			}
			if (Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent("Windows.Media.Core.TimedTextStyle", "IsUnderlineEnabled"))
			{
				copy->IsUnderlineEnabled = style->IsUnderlineEnabled;
			}

			return copy;
		}

		TimedTextRegion^ CopyRegion(TimedTextRegion^ region)
		{
			auto copy = ref new TimedTextRegion();
			copy->Background = region->Background;
			copy->DisplayAlignment = region->DisplayAlignment;
			copy->Extent = region->Extent;
			copy->IsOverflowClipped = region->IsOverflowClipped;
			copy->LineHeight = region->LineHeight;
			copy->Padding = region->Padding;
			copy->Position = region->Position;
			copy->ScrollMode = region->ScrollMode;
			copy->TextWrapping = region->TextWrapping;
			copy->WritingMode = region->WritingMode;
			copy->ZIndex = region->ZIndex;
			return copy;
		}

		TimedTextDisplayAlignment GetVerticalAlignment(int alignment, TimedTextDisplayAlignment defaultAlignment = TimedTextDisplayAlignment::Before)
		{
			// 1 2 3	bottom left center right
			// 4 5 8	top left
			// 6		top center
			// 7		top right
			// 9		center left
			// 10		center center
			// 11		center right
			if (isAss)
			{
				return alignment <= 3 ? TimedTextDisplayAlignment::After :
					alignment >= 9 ? TimedTextDisplayAlignment::Center :
					defaultAlignment;
			}
			else if (ssaVersion == 4)
			{
				return alignment <= 3 ? TimedTextDisplayAlignment::After :
					alignment <= 6 ? TimedTextDisplayAlignment::Center :
					defaultAlignment;
			}
			else
				return defaultAlignment;
		}

		TimedTextLineAlignment GetHorizontalAlignment(int alignment, TimedTextLineAlignment defaultAlignment = TimedTextLineAlignment::End)
		{
			// 1 2 3	bottom left center right
			// 4 5 8	top left
			// 6		top center
			// 7		top right
			// 9		center left
			// 10		center center
			// 11		center right
			if (isAss)
			{
				return alignment == 2 || alignment == 6 || alignment == 10 ? TimedTextLineAlignment::Center :
					alignment == 1 || alignment == 4 || alignment == 5 || alignment == 8 || alignment == 9 ? TimedTextLineAlignment::Start :
					defaultAlignment;
			}
			else if (ssaVersion == 4)
			{
				return alignment == 2 || alignment == 5 || alignment == 8 ? TimedTextLineAlignment::Center :
					alignment == 1 || alignment == 4 || alignment == 7 ? TimedTextLineAlignment::Start :
					defaultAlignment;
			}
			else
				return defaultAlignment;
		}

		String^ GetFontFamily(char* str)
		{
			auto wstr = utf8_to_wstring(std::string(str));
			return GetFontFamily(wstr);
		}

		String^ GetFontFamily(std::wstring str)
		{
			str = trim(str);

			auto existing = fonts.find(str);
			if (existing != fonts.end())
			{
				return existing->second;
			}

			String^ result;

			if (m_config->UseEmbeddedSubtitleFonts)
			{
				try
				{
					for each (auto attachment in attachedFileHelper->AttachedFiles)
					{
						std::wstring mime(attachment->MimeType->Data());
						if (mime.find(L"font") != mime.npos)
						{
							auto file = create_task(attachedFileHelper->ExtractFile(attachment)).get();
							auto names = ref new Vector<String^>();
							names->Append("System.Title");
							auto properties = create_task(file->Properties->RetrievePropertiesAsync(names)).get();
							auto title = dynamic_cast<String^>(properties->Lookup("System.Title"));
							if (title != nullptr)
							{
								auto fontFamily = std::wstring(title->Data());
								if (str.compare(fontFamily) == 0)
								{
									result = "ms-appdata:///temp/" + m_config->AttachmentCacheFolderName + "/" + attachedFileHelper->InstanceId + "/" + attachment->Name + "#" + convertFromString(str);
									break;
								}
							}
						}
					}
				}
				catch (...)
				{
				}
			}

			if (!result)
			{
				result = convertFromString(str);
			}

			fonts[str] = result;

			return result;
		}

		Windows::UI::Color ColorFromArgb(int argb)
		{
			auto result = *reinterpret_cast<Windows::UI::Color*>(&argb);
			return result;
		}

		void find_and_replace(std::wstring& source, std::wstring const& find, std::wstring const& replace)
		{
			for (std::wstring::size_type i = 0; (i = source.find(find, i)) != std::wstring::npos;)
			{
				source.replace(i, find.length(), replace);
				i += replace.length();
			}
		}

		// trim from left
		inline std::wstring& ltrim(std::wstring& s, const wchar_t* t = L" \t\n\r\f\v")
		{
			s.erase(0, s.find_first_not_of(t));
			return s;
		}

		// trim from right
		inline std::wstring& rtrim(std::wstring& s, const wchar_t* t = L" \t\n\r\f\v")
		{
			s.erase(s.find_last_not_of(t) + 1);
			return s;
		}

		// trim from left & right
		inline std::wstring& trim(std::wstring& s, const wchar_t* t = L" \t\n\r\f\v")
		{
			return ltrim(rtrim(s, t), t);
		}

		ref class SsaStyleDefinition
		{
		public:
			property String^ Name;
			property TimedTextRegion^ Region;
			property TimedTextStyle^ Style;
		};

	private:
		bool isAss;
		int ssaVersion;
		int textIndex;
		int width;
		int height;
		const int styleIndex = 2;
		std::map<String^, SsaStyleDefinition^> styles;
		std::map<std::wstring, String^> fonts;
		AttachedFileHelper^ attachedFileHelper;
	};
}